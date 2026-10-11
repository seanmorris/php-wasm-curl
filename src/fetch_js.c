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
		Module.libcurlFetch = {nextId: 1, requests: new Map(), waiters: new Set()};
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
	const request = state.requests.get(id);
	if(!request) return;
	request.cancelled = true;
	if(request.controller) request.controller.abort();
	if(request.resume) request.resume();
	state.requests.delete(id);
	for(const wake of Array.from(state.waiters)) wake();
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

/* Starts IO without suspending PHP. JS never invokes PHP callbacks while
   Asyncify is unwound: C consumes one event per handle and acknowledges it. */
EM_JS(void, lcf_js_start, (int id), {
	const state = Module.libcurlFetch;
	const CURLE_UNSUPPORTED_PROTOCOL = 1;
	const CURLE_URL_MALFORMAT = 3;
	const CURLE_COULDNT_CONNECT = 7;
	const CURLE_TOO_MANY_REDIRECTS = 47;
	const CURLE_OPERATION_TIMEDOUT = 28;
	const CURLE_RECV_ERROR = 56;

	const browser = typeof globalThis.window !== 'undefined'
		|| (typeof WorkerGlobalScope !== 'undefined' && globalThis instanceof WorkerGlobalScope);
	const canStopAtRedirect = !browser;

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

		const publish = event => {
			if(request.cancelled) return;
			request.event = event;
			for(const wake of Array.from(state.waiters)) wake();
		};
		const deliver = async (kind, body) => {
			await new Promise(resolve => {
				request.resume = resolve;
				publish({...result, kind, body: body || new Uint8Array(0), finished: performance.now()});
			});
			request.resume = null;
			if(request.cancelled || request.controller.signal.aborted) throw new DOMException('Aborted', 'AbortError');
		};
		const finish = () => publish({...result, kind: 3, finished: performance.now()});

		let parsed;
		try
		{
			parsed = new URL(request.url);
		}
		catch(error)
		{
			result.code = CURLE_URL_MALFORMAT;
			result.error = 'URL rejected: ' + request.url;
			return finish();
		}

		if(parsed.protocol !== 'http:' && parsed.protocol !== 'https:')
		{
			result.code = CURLE_UNSUPPORTED_PROTOCOL;
			result.error = 'Protocol "' + parsed.protocol.slice(0, -1) + '" not supported by php-wasm-curl';
			return finish();
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
			/* FormData string fields transcode UTF-8 and normalize line endings.
			   Serialize the envelope ourselves so every part keeps its raw bytes. */
			const boundary = '----php-wasm-curl-' + Math.random().toString(16).slice(2) + Math.random().toString(16).slice(2);
			const quoted = value => value.split(String.fromCharCode(13)).join('%0D')
				.split(String.fromCharCode(10)).join('%0A').split(String.fromCharCode(34)).join('%22');
			const chunks = [];
			for(const part of request.form)
			{
				let disposition = '--' + boundary + '\r\nContent-Disposition: form-data; name="' + quoted(part.name) + '"';
				if(part.filename !== undefined)
				{
					const type = new Blob([], {type: part.type}).type || 'application/octet-stream';
					disposition += '; filename="' + quoted(part.filename) + '"\r\nContent-Type: ' + type;
				}
				chunks.push(disposition + '\r\n\r\n', part.data, '\r\n');
			}
			chunks.push('--' + boundary + '--\r\n');
			body = new Blob(chunks);
			headers.set('content-type', 'multipart/form-data; boundary=' + boundary);
		}
		else if(request.body)
		{
			body = request.body;
		}

		const method = new Request(parsed.href, {method: request.method}).method;
		if(method === 'GET' || method === 'HEAD')
		{
			body = undefined;
		}

		let redirect = 'manual';
		if(!canStopAtRedirect)
		{
			redirect = request.follow && request.maxRedirs !== 0 ? 'follow' : 'manual';
		}

		const controller = request.controller = new AbortController();
		controller.signal.addEventListener('abort', () => { if(request.resume) request.resume(); });
		const init = {method, headers, body, redirect, signal: controller.signal};
		if(browser)
		{
			const policy = (Module.libcurlFetchOptions || {}).credentials || 'same-origin';
			if(!['omit', 'same-origin', 'include'].includes(policy))
			{
				result.code = 43;
				result.error = 'Invalid libcurlFetchOptions.credentials';
				return finish();
			}
			init.credentials = policy;
		}
		let timer = null;
		if(request.timeoutMs > 0) timer = setTimeout(() => controller.abort(), request.timeoutMs);

		try
		{
			let response;
			let currentURL = parsed;
			let followed = 0;
			let authAttempts = 0;
			for(;;)
			{
				response = await fetch(currentURL.href, init);
				const responseURL = new URL(response.url || currentURL.href);
				if(response.status === 401 && request.auth && responseURL.origin === parsed.origin && authAttempts < 2)
				{
					const challenge = response.headers.get('www-authenticate') || "";
					const digest = await state.digest(challenge, request.auth, init.method, responseURL, init.body);
					if(digest && (!authAttempts || digest.stale || responseURL.href !== currentURL.href))
					{
						if(response.body) await response.body.cancel();
						headers.set('authorization', digest.header);
						currentURL = responseURL;
						authAttempts++;
						continue;
					}
				}
				if(!canStopAtRedirect || !request.follow
					|| ![301, 302, 303, 307, 308].includes(response.status)) break;
				const location = response.headers.get('location');
				if(location === null) break;
				if(response.body) await response.body.cancel();
				if(request.maxRedirs >= 0 && followed >= request.maxRedirs)
				{
					result.code = CURLE_TOO_MANY_REDIRECTS;
					result.error = 'Maximum (' + request.maxRedirs + ') redirects followed';
					result.url = currentURL.href;
					return finish();
				}
				const nextURL = new URL(location, currentURL);
				if(nextURL.protocol !== 'http:' && nextURL.protocol !== 'https:')
				{
					result.code = CURLE_UNSUPPORTED_PROTOCOL;
					result.error = 'Redirect protocol is not supported by php-wasm-curl';
					return finish();
				}
				/* Match fetch's credential stripping and method/body transitions. */
				if(nextURL.origin !== currentURL.origin)
				{
					headers.delete('authorization');
					headers.delete('proxy-authorization');
					headers.delete('cookie');
				}
				const previousMethod = init.method.toUpperCase();
				if(((response.status === 301 || response.status === 302) && previousMethod === 'POST')
					|| (response.status === 303 && previousMethod !== 'GET' && previousMethod !== 'HEAD'))
				{
					init.method = 'GET';
					init.body = undefined;
					for(const name of ['content-encoding', 'content-language', 'content-location', 'content-type'])
					{
						headers.delete(name);
					}
				}
				currentURL = nextURL;
				if(request.auth) headers.delete('authorization');
				authAttempts = 0;
				followed++;
			}
			if(response.type === 'opaqueredirect')
			{
				result.code = request.follow ? CURLE_TOO_MANY_REDIRECTS : 4;
				result.error = request.follow ? 'Maximum (0) redirects followed'
					: 'Browser Fetch cannot expose a redirect response; redirect was not followed';
				return finish();
			}
			result.headersAt = performance.now();
			result.status = response.status;
			result.statusText = response.statusText;
			result.redirected = canStopAtRedirect ? followed : (response.redirected ? 1 : 0);
			result.url = response.url || parsed.href;

			const decoded = response.headers.has('content-encoding');
			const lines = [];
			for(const [name, value] of response.headers)
			{
				/* fetch() always decodes the body, so drop headers that describe the
				   encoded bytes; clients would otherwise try to decode it again. */
				if(decoded && name === 'content-encoding') continue;
				/* CORS may hide Content-Encoding while exposing the encoded length. */
				if((decoded || response.type === 'cors') && name === 'content-length') continue;
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

			result.headers = lines.join('\r\n') + (lines.length ? '\r\n' : "");
			await deliver(1);
			/* Backpressure bounds queued data and lets callbacks abort before
			   reading more. No JS callback re-enters suspended WebAssembly. */
			if(response.body)
			{
				const reader = response.body.getReader();
				try
				{
					for(;;)
					{
						const chunk = await reader.read();
						if(chunk.done) break;
						for(let offset = 0; offset < chunk.value.length; offset += 16384)
						{
							await deliver(2, chunk.value.subarray(offset, offset + 16384));
						}
					}
				}
				finally { reader.releaseLock(); }
			}
		}
		catch(error)
		{
			const message = error && error.message ? error.message : String(error);
			const cause = error && error.cause && error.cause.message ? ' (' + error.cause.message + ')' : "";
			if(error && error.curlCode)
			{
				result.code = error.curlCode;
				result.error = error.message;
			}
			else if(error && error.name === 'AbortError')
			{
				result.code = CURLE_OPERATION_TIMEDOUT;
				result.error = 'Operation timed out after ' + request.timeoutMs + ' milliseconds';
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
			result.finished = performance.now();
		}

		return finish();
	};

	/* All failures, including invalid Headers/Blob options, become curl errors. */
	run(id).catch(error => {
		const request = state.requests.get(id);
		if(!request || request.cancelled) return;
		request.event = {kind: 3, code: 7, error: String(error), started: performance.now(), finished: performance.now()};
		for(const wake of Array.from(state.waiters)) wake();
	});
});

EM_JS(void, lcf_js_continue, (int id), {
	const request = Module.libcurlFetch.requests.get(id);
	if(request && request.resume) request.resume();
});

EM_ASYNC_JS(int, lcf_js_wait, (const int *ids, int count, int timeout_ms), {
	const state = Module.libcurlFetch;
	if(!state || !count) return 0;
	const list = [];
	for(let i = 0; i < count; i++) list.push(getValue(ids + i * 4, 'i32'));
	const ready = () => list.filter(id => { const request = state.requests.get(id); return request && request.event; }).length;
	if(ready()) return ready();
	return await new Promise(resolve => {
		let timer;
		const wake = () => {
			const found = ready();
			if(!found && list.some(id => state.requests.has(id))) return;
			clearTimeout(timer);
			state.waiters.delete(wake);
			resolve(found);
		};
		state.waiters.add(wake);
		timer = setTimeout(() => { state.waiters.delete(wake); resolve(ready()); }, Math.max(0, timeout_ms));
		wake();
	});
});

/* Moves one event into C. It is acknowledged only after its callbacks return. */
EM_JS(int, lcf_js_take_event, (int id, int *code, long *status, long *redirected, char **status_text, char **url, char **headers, char **location, char **error, char **body, size_t *body_length, double *started, double *headers_at, double *finished), {
	const state = Module.libcurlFetch;
	const request = state.requests.get(id);
	if(!request || !request.event) return 0;
	const result = request.event;
	request.event = null;

	let outOfMemory = false;
	const toC = value => {
		if(!value) return 0;
		const length = lengthBytesUTF8(value) + 1;
		const pointer = _malloc(length);
		if(!pointer) { outOfMemory = true; return 0; }
		stringToUTF8(value, pointer, length);
		return pointer;
	};

	setValue(code, result.code || 0, 'i32');
	setValue(status, result.status || 0, 'i32');
	setValue(redirected, result.redirected || 0, 'i32');
	setValue(status_text, toC(result.kind === 1 ? result.statusText : null), '*');
	setValue(url, toC(result.url), '*');
	setValue(headers, toC(result.kind === 1 ? result.headers : null), '*');
	setValue(location, toC(result.location), '*');
	setValue(error, toC(result.error), '*');

	const bytes = result.body || new Uint8Array(0);
	const bodyPointer = bytes.length ? _malloc(bytes.length) : 0;
	if(bytes.length && bodyPointer) HEAPU8.set(bytes, bodyPointer);
	else if(bytes.length) outOfMemory = true;
	setValue(body, bodyPointer, '*');
	setValue(body_length, bytes.length, 'i32');

	setValue(started, result.started, 'double');
	setValue(headers_at, result.headersAt || result.started, 'double');
	setValue(finished, result.finished, 'double');
	if(outOfMemory) { setValue(code, 27, 'i32'); return 3; }
	return result.kind;
});
