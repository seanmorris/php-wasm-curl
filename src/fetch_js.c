/*
 * The JavaScript half of php-wasm-curl.
 *
 * EM_JS bodies are stringified by the C preprocessor, which joins every line:
 * end each statement with a semicolon, write empty strings with double quotes,
 * keep each parameter list on one line, and never write two question marks in
 * a row (C trigraphs).
 */
#include <emscripten.h>
#include "internal.h"

/* EM_JS only declares imports, so nothing else would pull this object out of
   libcurl.a. lcf_prepare() references this to keep it linked. */
int lcf_js_anchor = 1;

EM_JS(int, lcf_js_request_new, (void), {
	if(!Module.libcurlFetch)
	{
		Module.libcurlFetch = {nextId: 1, requests: new Map(), results: new Map()};
	}
	const state = Module.libcurlFetch;
	const id = state.nextId++;
	state.requests.set(id, {
		url: "", method: 'GET', headers: [], body: null, form: null,
		follow: false, maxRedirs: -1, timeoutMs: 0, port: 0
	});
	return id;
});

EM_JS(void, lcf_js_request_free, (int id), {
	const state = Module.libcurlFetch;
	if(!state) return;
	state.requests.delete(id);
	state.results.delete(id);
});

EM_JS(void, lcf_js_request_url, (int id, const char *url), {
	Module.libcurlFetch.requests.get(id).url = UTF8ToString(url);
});

EM_JS(void, lcf_js_request_method, (int id, const char *method), {
	Module.libcurlFetch.requests.get(id).method = UTF8ToString(method);
});

EM_JS(void, lcf_js_request_header, (int id, const char *name, const char *value), {
	Module.libcurlFetch.requests.get(id).headers.push([UTF8ToString(name), UTF8ToString(value)]);
});

EM_JS(void, lcf_js_request_body, (int id, const char *body, size_t length), {
	Module.libcurlFetch.requests.get(id).body = HEAPU8.slice(body, body + length);
});

EM_JS(void, lcf_js_request_form_field, (int id, const char *name, const char *data, size_t length), {
	const request = Module.libcurlFetch.requests.get(id);
	if(!request.form) request.form = [];
	request.form.push({name: UTF8ToString(name), data: HEAPU8.slice(data, data + length)});
});

EM_JS(void, lcf_js_request_form_file, (int id, const char *name, const char *data, size_t length, const char *filename, const char *type), {
	const request = Module.libcurlFetch.requests.get(id);
	if(!request.form) request.form = [];
	request.form.push({
		name: UTF8ToString(name),
		data: HEAPU8.slice(data, data + length),
		filename: UTF8ToString(filename),
		type: type ? UTF8ToString(type) : 'application/octet-stream'
	});
});

EM_JS(void, lcf_js_request_options, (int id, int follow, long max_redirs, long timeout_ms, long port), {
	const request = Module.libcurlFetch.requests.get(id);
	request.port = port;
	request.follow = !!follow;
	request.maxRedirs = max_redirs;
	request.timeoutMs = timeout_ms;
});

/*
 * Runs every listed request concurrently and stores one result per id. Never
 * rejects: failures become results carrying a libcurl error code.
 */
EM_ASYNC_JS(void, lcf_js_fetch_all, (const int *ids, int count), {
	const state = Module.libcurlFetch;
	const CURLE_UNSUPPORTED_PROTOCOL = 1;
	const CURLE_URL_MALFORMAT = 3;
	const CURLE_COULDNT_CONNECT = 7;
	const CURLE_TOO_MANY_REDIRECTS = 47;
	const CURLE_OPERATION_TIMEDOUT = 28;
	const CURLE_RECV_ERROR = 56;

	/* Browsers hide redirect responses from fetch() ("opaqueredirect"), so
	   there a redirect can only be followed. Server runtimes expose them. */
	const canStopAtRedirect = !!(globalThis.process || globalThis.Deno || globalThis.Bun
		|| (globalThis.navigator && globalThis.navigator.userAgent === 'Cloudflare-Workers'));

	/* fetch() computes or forbids these itself. */
	const skippedHeaders = new Set([
		'host', 'content-length', 'connection', 'keep-alive', 'transfer-encoding',
		'upgrade', 'expect', 'proxy-connection', 'te', 'trailer'
	]);

	const run = async id => {
		const request = state.requests.get(id);
		const started = performance.now();
		const result = {code: 0, status: 0, redirected: 0, statusText: "", url: request.url,
			headers: "", location: "", error: "", body: new Uint8Array(0),
			started, headersAt: started, finished: started};

		let parsed;
		try
		{
			parsed = new URL(request.url);
		}
		catch(error)
		{
			result.code = CURLE_URL_MALFORMAT;
			result.error = 'URL rejected: ' + request.url;
			return result;
		}

		if(parsed.protocol !== 'http:' && parsed.protocol !== 'https:')
		{
			result.code = CURLE_UNSUPPORTED_PROTOCOL;
			result.error = 'Protocol "' + parsed.protocol.slice(0, -1) + '" not supported by php-wasm-curl';
			return result;
		}

		if(request.port > 0)
		{
			parsed.port = String(request.port);
		}

		const headers = new Headers();
		for(const [name, value] of request.headers)
		{
			if(skippedHeaders.has(name.toLowerCase())) continue;
			try
			{
				headers.append(name, value);
			}
			catch(error)
			{}
		}

		let body = undefined;
		if(request.form)
		{
			body = new FormData();
			for(const part of request.form)
			{
				if(part.filename === undefined)
				{
					body.append(part.name, new TextDecoder().decode(part.data));
				}
				else
				{
					body.append(part.name, new Blob([part.data], {type: part.type}), part.filename);
				}
			}
			headers.delete('content-type');
		}
		else if(request.body)
		{
			body = request.body;
		}

		const method = request.method.toUpperCase();
		if(method === 'GET' || method === 'HEAD')
		{
			body = undefined;
		}

		let redirect = 'manual';
		if(request.follow || !canStopAtRedirect)
		{
			redirect = request.follow && request.maxRedirs === 0 ? 'error' : 'follow';
		}

		const init = {method: request.method, headers, body, redirect};
		let timer = null;
		if(request.timeoutMs > 0)
		{
			const controller = new AbortController();
			timer = setTimeout(() => controller.abort(), request.timeoutMs);
			init.signal = controller.signal;
		}

		try
		{
			const response = await fetch(parsed.href, init);
			result.headersAt = performance.now();
			result.status = response.status;
			result.statusText = response.statusText;
			result.redirected = response.redirected ? 1 : 0;
			result.url = response.url || parsed.href;

			const decoded = response.headers.has('content-encoding');
			const lines = [];
			for(const [name, value] of response.headers)
			{
				/* fetch() always decodes the body, so drop headers that describe the
				   encoded bytes; clients would otherwise try to decode it again. */
				if(decoded && (name === 'content-encoding' || name === 'content-length')) continue;
				if(name === 'set-cookie') continue;
				lines.push(name + ': ' + value);
			}
			const cookies = response.headers.getSetCookie ? response.headers.getSetCookie() : [];
			for(const cookie of cookies)
			{
				lines.push('set-cookie: ' + cookie);
			}

			if(response.status >= 300 && response.status < 400)
			{
				const location = response.headers.get('location');
				result.location = location ? new URL(location, result.url).href : "";
			}

			const buffer = new Uint8Array(await response.arrayBuffer());
			if(decoded)
			{
				lines.push('content-length: ' + buffer.length);
			}
			result.headers = lines.join('\r\n') + (lines.length ? '\r\n' : "");
			result.body = buffer;
		}
		catch(error)
		{
			const message = error && error.message ? error.message : String(error);
			const cause = error && error.cause && error.cause.message ? ' (' + error.cause.message + ')' : "";
			if(error && error.name === 'AbortError')
			{
				result.code = CURLE_OPERATION_TIMEDOUT;
				result.error = 'Operation timed out after ' + request.timeoutMs + ' milliseconds';
			}
			else if(redirect === 'error' && /redirect/i.test(message + cause))
			{
				result.code = CURLE_TOO_MANY_REDIRECTS;
				result.error = 'Maximum (0) redirects followed';
			}
			else if(result.status)
			{
				result.code = CURLE_RECV_ERROR;
				result.error = 'Failure when receiving data from the peer: ' + message + cause;
			}
			else
			{
				result.code = CURLE_COULDNT_CONNECT;
				result.error = 'Failed to connect to ' + parsed.host + ': ' + message + cause;
			}
		}
		finally
		{
			if(timer) clearTimeout(timer);
		}

		result.finished = performance.now();
		return result;
	};

	const list = [];
	for(let i = 0; i < count; i++)
	{
		list.push(getValue(ids + i * 4, 'i32'));
	}

	const results = await Promise.all(list.map(run));
	list.forEach((id, i) => state.results.set(id, results[i]));
});

/* Moves one stored result into C. Every returned string or buffer is malloc'd. */
EM_JS(void, lcf_js_take_result, (int id, int *code, long *status, long *redirected, char **status_text, char **url, char **headers, char **location, char **error, char **body, size_t *body_length, double *started, double *headers_at, double *finished), {
	const state = Module.libcurlFetch;
	const result = state.results.get(id);
	state.results.delete(id);

	const toC = value => {
		const length = lengthBytesUTF8(value) + 1;
		const pointer = _malloc(length);
		stringToUTF8(value, pointer, length);
		return pointer;
	};

	setValue(code, result.code, 'i32');
	setValue(status, result.status, 'i32');
	setValue(redirected, result.redirected, 'i32');
	setValue(status_text, toC(result.statusText), '*');
	setValue(url, toC(result.url), '*');
	setValue(headers, toC(result.headers), '*');
	setValue(location, toC(result.location), '*');
	setValue(error, toC(result.error), '*');

	const bodyPointer = _malloc(result.body.length + 1);
	HEAPU8.set(result.body, bodyPointer);
	HEAPU8[bodyPointer + result.body.length] = 0;
	setValue(body, bodyPointer, '*');
	setValue(body_length, result.body.length, 'i32');

	setValue(started, result.started, 'double');
	setValue(headers_at, result.headersAt, 'double');
	setValue(finished, result.finished, 'double');
});
