#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "internal.h"

#define LCF_CHUNK 16384

typedef struct lcf_header {
	char *name;
	char *value;
} lcf_header;

typedef struct lcf_header_list {
	lcf_header *items;
	size_t count;
} lcf_header_list;

static void lcf_headers_remove(lcf_header_list *list, const char *name, size_t name_length)
{
	size_t kept = 0;
	for(size_t i = 0; i < list->count; i++)
	{
		if(strlen(list->items[i].name) == name_length && strncasecmp(list->items[i].name, name, name_length) == 0)
		{
			free(list->items[i].name);
			free(list->items[i].value);
			continue;
		}
		list->items[kept++] = list->items[i];
	}
	list->count = kept;
}

static int lcf_headers_set(lcf_header_list *list, const char *name, size_t name_length, const char *value)
{
	lcf_headers_remove(list, name, name_length);
	lcf_header *items = realloc(list->items, (list->count + 1) * sizeof(lcf_header));
	if(!items) return 0;
	list->items = items;
	list->items[list->count].name = strndup(name, name_length);
	list->items[list->count].value = lcf_strdup(value);
	list->count++;
	return 1;
}

static void lcf_headers_free(lcf_header_list *list)
{
	for(size_t i = 0; i < list->count; i++)
	{
		free(list->items[i].name);
		free(list->items[i].value);
	}
	free(list->items);
	list->items = NULL;
	list->count = 0;
}

static int lcf_has_header(struct curl_slist *list, const char *name)
{
	size_t length = strlen(name);
	for(; list; list = list->next)
	{
		if(strncasecmp(list->data, name, length) == 0 && (list->data[length] == ':' || list->data[length] == ';'))
		{
			return 1;
		}
	}
	return 0;
}

static char *lcf_base64(const char *input)
{
	static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	size_t length = strlen(input);
	char *out = malloc(((length + 2) / 3) * 4 + 1);
	if(!out) return NULL;
	size_t o = 0;
	for(size_t i = 0; i < length; i += 3)
	{
		unsigned int n = (unsigned char)input[i] << 16;
		if(i + 1 < length) n |= (unsigned char)input[i + 1] << 8;
		if(i + 2 < length) n |= (unsigned char)input[i + 2];
		out[o++] = table[(n >> 18) & 63];
		out[o++] = table[(n >> 12) & 63];
		out[o++] = i + 1 < length ? table[(n >> 6) & 63] : '=';
		out[o++] = i + 2 < length ? table[n & 63] : '=';
	}
	out[o] = '\0';
	return out;
}

static CURLcode lcf_read_all(curl_read_callback read_cb, void *arg, curl_off_t limit, lcf_buffer *out)
{
	char chunk[LCF_CHUNK];
	for(;;)
	{
		size_t wanted = LCF_CHUNK;
		if(limit >= 0)
		{
			if((curl_off_t)out->length >= limit) break;
			if((curl_off_t)(out->length + wanted) > limit) wanted = (size_t)(limit - out->length);
		}
		size_t got = read_cb(chunk, 1, wanted, arg);
		if(got == CURL_READFUNC_ABORT) return CURLE_ABORTED_BY_CALLBACK;
		if(got == CURL_READFUNC_PAUSE) return CURLE_READ_ERROR;
		if(got == 0) break;
		if(got > wanted) return CURLE_READ_ERROR;
		if(!lcf_buffer_append(out, chunk, got)) return CURLE_OUT_OF_MEMORY;
	}
	return CURLE_OK;
}

static const char *lcf_method(Curl_easy *data)
{
	if(data->custom_request) return data->custom_request;
	if(data->nobody) return "HEAD";
	if(data->upload) return "PUT";
	if(data->post) return "POST";
	return "GET";
}

/* Sends the request line and headers to CURLOPT_DEBUGFUNCTION, which is how
   PHP implements CURLINFO_HEADER_OUT. */
static void lcf_debug_request(Curl_easy *data, const char *method, lcf_header_list *headers)
{
	lcf_buffer text = {0};
	const char *url = data->url;
	const char *scheme_end = strstr(url, "://");
	const char *host = scheme_end ? scheme_end + 3 : url;
	const char *path = strchr(host, '/');
	size_t host_length = path ? (size_t)(path - host) : strlen(host);

	lcf_buffer_append(&text, method, strlen(method));
	lcf_buffer_append(&text, " ", 1);
	lcf_buffer_append(&text, path ? path : "/", path ? strlen(path) : 1);
	lcf_buffer_append(&text, " HTTP/1.1\r\nHost: ", 17);
	lcf_buffer_append(&text, host, host_length);
	lcf_buffer_append(&text, "\r\n", 2);
	for(size_t i = 0; i < headers->count; i++)
	{
		lcf_buffer_append(&text, headers->items[i].name, strlen(headers->items[i].name));
		lcf_buffer_append(&text, ": ", 2);
		lcf_buffer_append(&text, headers->items[i].value, strlen(headers->items[i].value));
		lcf_buffer_append(&text, "\r\n", 2);
	}
	lcf_buffer_append(&text, "\r\n", 2);

	data->request_size = (long)text.length;
	if(data->verbose && data->debug_cb && text.data)
	{
		data->debug_cb(data, CURLINFO_HEADER_OUT, text.data, text.length, data->debug_data);
	}
	lcf_buffer_free(&text);
}

static CURLcode lcf_add_mime(Curl_easy *data, int id)
{
	for(curl_mimepart *part = data->mime->first; part; part = part->next)
	{
		const char *name = part->name ? part->name : "";
		lcf_buffer content = {0};
		const char *bytes = part->data;
		size_t length = part->length;

		if(part->read_cb)
		{
			if(part->seek_cb) part->seek_cb(part->cb_arg, 0, SEEK_SET);
			CURLcode code = lcf_read_all(part->read_cb, part->cb_arg, part->cb_size, &content);
			if(code != CURLE_OK)
			{
				lcf_buffer_free(&content);
				return code;
			}
			bytes = content.data ? content.data : "";
			length = content.length;
		}

		if(part->filename)
		{
			lcf_js_request_form_file(id, name, bytes ? bytes : "", length, part->filename, part->type);
		}
		else
		{
			lcf_js_request_form_field(id, name, bytes ? bytes : "", length);
		}
		data->size_upload += length;
		lcf_buffer_free(&content);
	}
	return CURLE_OK;
}

/* Builds a fetch request in JavaScript from the handle's options. */
CURLcode lcf_prepare(Curl_easy *data)
{
	if(!lcf_js_anchor) return CURLE_FAILED_INIT;
	lcf_clear_results(data);

	if(!data->url || !*data->url)
	{
		lcf_set_error(data, CURLE_URL_MALFORMAT, "No URL set");
		return CURLE_URL_MALFORMAT;
	}
	if(data->proxy && *data->proxy)
	{
		lcf_set_error(data, CURLE_COULDNT_RESOLVE_PROXY, "Proxies are not supported by php-wasm-curl");
		return CURLE_COULDNT_RESOLVE_PROXY;
	}
	if(data->unix_socket && *data->unix_socket)
	{
		lcf_set_error(data, CURLE_NOT_BUILT_IN, "Unix sockets are not supported by php-wasm-curl");
		return CURLE_NOT_BUILT_IN;
	}

	const char *method = lcf_method(data);
	lcf_header_list headers = {0};
	lcf_buffer body = {0};
	bool has_body = false;
	CURLcode code = CURLE_OK;

	if(data->user_agent) lcf_headers_set(&headers, "User-Agent", 10, data->user_agent);
	if(data->referer) lcf_headers_set(&headers, "Referer", 7, data->referer);
	if(data->cookie) lcf_headers_set(&headers, "Cookie", 6, data->cookie);
	if(data->range)
	{
		char range[256];
		snprintf(range, sizeof(range), "bytes=%s", data->range);
		lcf_headers_set(&headers, "Range", 5, range);
	}
	if(data->bearer && (data->http_auth & CURLAUTH_BEARER))
	{
		size_t length = strlen(data->bearer) + 8;
		char *value = malloc(length);
		snprintf(value, length, "Bearer %s", data->bearer);
		lcf_headers_set(&headers, "Authorization", 13, value);
		free(value);
	}
	else if(data->username)
	{
		size_t length = strlen(data->username) + (data->password ? strlen(data->password) : 0) + 2;
		char *pair = malloc(length);
		snprintf(pair, length, "%s:%s", data->username, data->password ? data->password : "");
		char *encoded = lcf_base64(pair);
		size_t header_length = strlen(encoded) + 7;
		char *value = malloc(header_length);
		snprintf(value, header_length, "Basic %s", encoded);
		lcf_headers_set(&headers, "Authorization", 13, value);
		free(value);
		free(encoded);
		free(pair);
	}

	if(data->mime)
	{
		has_body = true;
	}
	else if(data->post_fields && (data->post || data->custom_request))
	{
		size_t length = data->post_field_size >= 0 ? (size_t)data->post_field_size : strlen(data->post_fields);
		if(!lcf_buffer_append(&body, data->post_fields, length)) code = CURLE_OUT_OF_MEMORY;
		has_body = true;
	}
	else if((data->upload || data->post) && data->read_cb)
	{
		curl_off_t limit = data->upload ? data->in_file_size : data->post_field_size;
		code = lcf_read_all(data->read_cb, data->read_data, limit, &body);
		has_body = true;
	}
	else if(data->post)
	{
		has_body = true;
	}

	if(has_body && !data->mime && !lcf_has_header(data->http_headers, "Content-Type") && data->post)
	{
		lcf_headers_set(&headers, "Content-Type", 12, "application/x-www-form-urlencoded");
	}

	/* CURLOPT_HTTPHEADER: "Name: value" sets, "Name:" removes, "Name;" sends empty. */
	for(struct curl_slist *item = data->http_headers; item; item = item->next)
	{
		const char *line = item->data;
		const char *colon = strchr(line, ':');
		const char *semicolon = strchr(line, ';');
		if(colon && (!semicolon || colon < semicolon))
		{
			const char *value = colon + 1;
			while(*value == ' ' || *value == '\t') value++;
			if(*value)
			{
				lcf_headers_set(&headers, line, colon - line, value);
			}
			else
			{
				lcf_headers_remove(&headers, line, colon - line);
			}
		}
		else if(semicolon && semicolon[1] == '\0')
		{
			lcf_headers_set(&headers, line, semicolon - line, "");
		}
	}

	if(code != CURLE_OK)
	{
		lcf_headers_free(&headers);
		lcf_buffer_free(&body);
		lcf_set_error(data, code, NULL);
		return code;
	}

	int id = lcf_js_request_new();
	data->request_id = id;
	lcf_js_request_url(id, data->url);
	lcf_js_request_method(id, method);
	for(size_t i = 0; i < headers.count; i++)
	{
		lcf_js_request_header(id, headers.items[i].name, headers.items[i].value);
	}

	if(data->mime)
	{
		code = lcf_add_mime(data, id);
	}
	else if(has_body)
	{
		lcf_js_request_body(id, body.data ? body.data : "", body.length);
		data->size_upload = (curl_off_t)body.length;
	}
	data->content_length_upload = has_body ? data->size_upload : -1;

	lcf_js_request_options(id, data->follow_location, data->max_redirs, data->timeout_ms, data->port);
	lcf_debug_request(data, method, &headers);
	data->request_size += (long)data->size_upload;

	lcf_headers_free(&headers);
	lcf_buffer_free(&body);

	if(code != CURLE_OK)
	{
		lcf_js_request_free(id);
		data->request_id = 0;
		lcf_set_error(data, code, NULL);
	}
	return code;
}

void lcf_free_result(lcf_result *result)
{
	free(result->status_text);
	free(result->url);
	free(result->headers);
	free(result->location);
	free(result->error);
	free(result->body);
	memset(result, 0, sizeof(lcf_result));
}

static CURLcode lcf_deliver_header(Curl_easy *data, const char *line, size_t length)
{
	data->header_size += (long)length;
	if(data->header_cb)
	{
		if(data->header_cb((char *)line, 1, length, data->header_data) != length) return CURLE_WRITE_ERROR;
	}
	if(data->header_in_body)
	{
		if(data->write_cb)
		{
			if(data->write_cb((char *)line, 1, length, data->write_data) != length) return CURLE_WRITE_ERROR;
		}
		else
		{
			fwrite(line, 1, length, data->write_data ? data->write_data : stdout);
		}
	}
	return CURLE_OK;
}

static CURLcode lcf_progress(Curl_easy *data)
{
	if(data->no_progress) return CURLE_OK;
	if(data->xferinfo_cb)
	{
		if(data->xferinfo_cb(data->xferinfo_data, data->size_download, data->size_download, data->size_upload, data->size_upload))
		{
			return CURLE_ABORTED_BY_CALLBACK;
		}
	}
	else if(data->progress_cb)
	{
		if(data->progress_cb(data->progress_data, (double)data->size_download, (double)data->size_download, (double)data->size_upload, (double)data->size_upload))
		{
			return CURLE_ABORTED_BY_CALLBACK;
		}
	}
	return CURLE_OK;
}

/* Copies the JavaScript result into the handle and replays it through the
   libcurl callbacks, the way a real transfer would deliver it. */
CURLcode lcf_finish(Curl_easy *data, lcf_result *result)
{
	data->total_time = (result->finished - result->started) / 1000.0;
	data->starttransfer_time = (result->headers_at - result->started) / 1000.0;
	lcf_set_string(&data->effective_url, result->url && *result->url ? result->url : data->url);

	if(result->code != CURLE_OK)
	{
		lcf_set_error(data, result->code, result->error);
		return result->code;
	}

	data->response_code = result->status;
	data->http_version = CURL_HTTP_VERSION_1_1;
	data->redirect_count = result->redirected;
	if(result->location && *result->location) lcf_set_string(&data->redirect_url, result->location);

	if(data->fail_on_error && result->status >= 400)
	{
		char message[CURL_ERROR_SIZE];
		snprintf(message, sizeof(message), "The requested URL returned error: %ld", result->status);
		lcf_set_error(data, CURLE_HTTP_RETURNED_ERROR, message);
		return CURLE_HTTP_RETURNED_ERROR;
	}

	char status_line[512];
	int status_length = snprintf(status_line, sizeof(status_line), "HTTP/1.1 %ld %s\r\n",
		result->status, result->status_text ? result->status_text : "");
	CURLcode code = lcf_deliver_header(data, status_line, (size_t)status_length);

	for(char *line = result->headers; code == CURLE_OK && line && *line;)
	{
		char *end = strstr(line, "\r\n");
		size_t length = end ? (size_t)(end - line) + 2 : strlen(line);

		if(strncasecmp(line, "content-type:", 13) == 0)
		{
			const char *value = line + 13;
			while(*value == ' ') value++;
			free(data->content_type);
			data->content_type = strndup(value, (line + length) - value - (end ? 2 : 0));
		}
		else if(strncasecmp(line, "content-length:", 15) == 0)
		{
			data->content_length_download = strtoll(line + 15, NULL, 10);
		}

		code = lcf_deliver_header(data, line, length);
		line += length;
	}

	if(code == CURLE_OK) code = lcf_deliver_header(data, "\r\n", 2);

	if(code == CURLE_OK && !data->nobody)
	{
		for(size_t offset = 0; offset < result->body_length; offset += LCF_CHUNK)
		{
			size_t length = result->body_length - offset;
			if(length > LCF_CHUNK) length = LCF_CHUNK;
			size_t written;
			if(data->write_cb)
			{
				written = data->write_cb(result->body + offset, 1, length, data->write_data);
			}
			else
			{
				written = fwrite(result->body + offset, 1, length, data->write_data ? data->write_data : stdout);
			}
			if(written != length)
			{
				code = CURLE_WRITE_ERROR;
				break;
			}
			data->size_download += (curl_off_t)length;
		}
	}

	if(code == CURLE_OK) code = lcf_progress(data);
	if(code != CURLE_OK) lcf_set_error(data, code, NULL);
	return code;
}

CURLcode lcf_take(Curl_easy *data, lcf_result *result)
{
	int code = 0;
	memset(result, 0, sizeof(lcf_result));
	lcf_js_take_result(
		data->request_id, &code, &result->status, &result->redirected, &result->status_text,
		&result->url, &result->headers, &result->location, &result->error, &result->body,
		&result->body_length, &result->started, &result->headers_at, &result->finished
	);
	result->code = (CURLcode)code;
	lcf_js_request_free(data->request_id);
	data->request_id = 0;
	return result->code;
}

CURL_EXTERN CURLcode curl_easy_perform(CURL *curl)
{
	Curl_easy *data = curl;
	if(!data || data->magic != LCF_EASY_MAGIC) return CURLE_BAD_FUNCTION_ARGUMENT;
	if(data->multi) return CURLE_FAILED_INIT;

	CURLcode code = lcf_prepare(data);
	if(code != CURLE_OK) return code;

	int id = data->request_id;
	lcf_js_fetch_all(&id, 1);

	lcf_result result;
	lcf_take(data, &result);
	code = lcf_finish(data, &result);
	lcf_free_result(&result);
	return code;
}
