#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "internal.h"

char *lcf_strdup(const char *value)
{
	if(!value) return NULL;
	size_t length = strlen(value);
	char *copy = malloc(length + 1);
	if(copy) memcpy(copy, value, length + 1);
	return copy;
}

void lcf_set_string(char **slot, const char *value)
{
	free(*slot);
	*slot = lcf_strdup(value);
}

int lcf_buffer_append(lcf_buffer *buffer, const char *data, size_t length)
{
	if(buffer->length + length + 1 > buffer->capacity)
	{
		size_t capacity = buffer->capacity ? buffer->capacity : 256;
		while(capacity < buffer->length + length + 1) capacity *= 2;
		char *grown = realloc(buffer->data, capacity);
		if(!grown) return 0;
		buffer->data = grown;
		buffer->capacity = capacity;
	}
	memcpy(buffer->data + buffer->length, data, length);
	buffer->length += length;
	buffer->data[buffer->length] = '\0';
	return 1;
}

void lcf_buffer_free(lcf_buffer *buffer)
{
	free(buffer->data);
	buffer->data = NULL;
	buffer->length = buffer->capacity = 0;
}

void lcf_clear_results(Curl_easy *data)
{
	data->response_code = 0;
	data->http_version = 0;
	data->redirect_count = 0;
	data->header_size = 0;
	data->request_size = 0;
	lcf_set_string(&data->effective_url, NULL);
	lcf_set_string(&data->content_type, NULL);
	lcf_set_string(&data->redirect_url, NULL);
	data->size_download = 0;
	data->size_upload = 0;
	data->content_length_download = -1;
	data->content_length_upload = -1;
	data->total_time = 0;
	data->starttransfer_time = 0;
}

void lcf_set_error(Curl_easy *data, CURLcode code, const char *message)
{
	if(data->error_buffer)
	{
		snprintf(data->error_buffer, CURL_ERROR_SIZE, "%s", message ? message : curl_easy_strerror(code));
	}
}

static void lcf_default_options(Curl_easy *data)
{
	data->magic = LCF_EASY_MAGIC;
	data->max_redirs = 30;
	data->no_progress = true;
	data->post_field_size = -1;
	data->in_file_size = -1;
	data->content_length_download = -1;
	data->content_length_upload = -1;
	data->request_id = 0;
}

static void lcf_free_options(Curl_easy *data)
{
	free(data->url);
	free(data->custom_request);
	free(data->user_agent);
	free(data->referer);
	free(data->cookie);
	free(data->range);
	free(data->username);
	free(data->password);
	free(data->bearer);
	free(data->proxy);
	free(data->unix_socket);
	free(data->post_fields_copy);
	lcf_clear_results(data);
}

CURL_EXTERN CURLcode curl_global_init(long flags)
{
	(void)flags;
	return CURLE_OK;
}

CURL_EXTERN void curl_global_cleanup(void)
{
}

CURL_EXTERN void curl_free(void *p)
{
	free(p);
}

CURL_EXTERN CURL *curl_easy_init(void)
{
	Curl_easy *data = calloc(1, sizeof(Curl_easy));
	if(!data) return NULL;
	lcf_default_options(data);
	return data;
}

CURL_EXTERN void curl_easy_cleanup(CURL *curl)
{
	Curl_easy *data = curl;
	if(!data || data->magic != LCF_EASY_MAGIC) return;
	if(data->multi) curl_multi_remove_handle(data->multi, data);
	lcf_free_options(data);
	data->magic = 0;
	free(data);
}

CURL_EXTERN void curl_easy_reset(CURL *curl)
{
	Curl_easy *data = curl;
	if(!data || data->magic != LCF_EASY_MAGIC) return;
	struct Curl_multi *multi = data->multi;
	struct Curl_easy *next = data->next_in_multi;
	lcf_free_options(data);
	memset(data, 0, sizeof(Curl_easy));
	lcf_default_options(data);
	data->multi = multi;
	data->next_in_multi = next;
}

CURL_EXTERN CURL *curl_easy_duphandle(CURL *curl)
{
	Curl_easy *source = curl;
	if(!source || source->magic != LCF_EASY_MAGIC) return NULL;
	Curl_easy *copy = malloc(sizeof(Curl_easy));
	if(!copy) return NULL;
	memcpy(copy, source, sizeof(Curl_easy));

	char **strings[] = {
		&copy->url, &copy->custom_request, &copy->user_agent, &copy->referer, &copy->cookie,
		&copy->range, &copy->username, &copy->password, &copy->bearer, &copy->proxy,
		&copy->unix_socket, &copy->effective_url, &copy->content_type, &copy->redirect_url
	};
	for(size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++)
	{
		*strings[i] = lcf_strdup(*strings[i]);
	}

	if(source->post_fields_copy)
	{
		size_t length = source->post_field_size >= 0 ? (size_t)source->post_field_size : strlen(source->post_fields_copy);
		copy->post_fields_copy = malloc(length + 1);
		memcpy(copy->post_fields_copy, source->post_fields_copy, length + 1);
		copy->post_fields = copy->post_fields_copy;
	}

	copy->multi = NULL;
	copy->next_in_multi = NULL;
	copy->done = false;
	copy->request_id = 0;
	return copy;
}

static CURLcode lcf_setopt_long(Curl_easy *data, CURLoption option, long value)
{
	switch(option)
	{
		case CURLOPT_POST:
			data->post = value != 0;
			if(data->post) { data->nobody = false; data->upload = false; }
			break;
		case CURLOPT_HTTPGET:
			if(value)
			{
				data->post = false;
				data->nobody = false;
				data->upload = false;
				data->mime = NULL;
				data->post_fields = NULL;
			}
			break;
		case CURLOPT_NOBODY:
			data->nobody = value != 0;
			break;
		case CURLOPT_UPLOAD:
			data->upload = value != 0;
			if(data->upload) { data->post = false; data->nobody = false; }
			break;
		case CURLOPT_HEADER:
			data->header_in_body = value != 0;
			break;
		case CURLOPT_FOLLOWLOCATION:
			data->follow_location = value != 0;
			break;
		case CURLOPT_MAXREDIRS:
			data->max_redirs = value;
			break;
		case CURLOPT_FAILONERROR:
			data->fail_on_error = value != 0;
			break;
		case CURLOPT_VERBOSE:
			data->verbose = value != 0;
			break;
		case CURLOPT_NOPROGRESS:
			data->no_progress = value != 0;
			break;
		case CURLOPT_TIMEOUT:
			data->timeout_ms = value * 1000;
			break;
		case CURLOPT_TIMEOUT_MS:
			data->timeout_ms = value;
			break;
		case CURLOPT_INFILESIZE:
			data->in_file_size = value;
			break;
		case CURLOPT_POSTFIELDSIZE:
			data->post_field_size = value;
			break;
		case CURLOPT_PORT:
			data->port = value;
			break;
		case CURLOPT_HTTPAUTH:
			data->http_auth = value;
			break;
		default:
			/* Connection, TLS, DNS and timing knobs have no fetch() equivalent. */
			break;
	}
	return CURLE_OK;
}

static CURLcode lcf_setopt_pointer(Curl_easy *data, CURLoption option, void *value)
{
	switch(option)
	{
		case CURLOPT_URL: lcf_set_string(&data->url, value); break;
		case CURLOPT_CUSTOMREQUEST: lcf_set_string(&data->custom_request, value); break;
		case CURLOPT_USERAGENT: lcf_set_string(&data->user_agent, value); break;
		case CURLOPT_REFERER: lcf_set_string(&data->referer, value); break;
		case CURLOPT_COOKIE: lcf_set_string(&data->cookie, value); break;
		case CURLOPT_RANGE: lcf_set_string(&data->range, value); break;
		case CURLOPT_USERNAME: lcf_set_string(&data->username, value); break;
		case CURLOPT_PASSWORD: lcf_set_string(&data->password, value); break;
		case CURLOPT_XOAUTH2_BEARER: lcf_set_string(&data->bearer, value); break;
		case CURLOPT_PROXY: lcf_set_string(&data->proxy, value); break;
		case CURLOPT_UNIX_SOCKET_PATH: lcf_set_string(&data->unix_socket, value); break;
		case CURLOPT_USERPWD:
		{
			const char *userpwd = value;
			free(data->username);
			free(data->password);
			data->username = data->password = NULL;
			if(userpwd)
			{
				const char *colon = strchr(userpwd, ':');
				if(colon)
				{
					data->username = strndup(userpwd, colon - userpwd);
					data->password = lcf_strdup(colon + 1);
				}
				else
				{
					data->username = lcf_strdup(userpwd);
				}
			}
			break;
		}
		case CURLOPT_HTTPHEADER: data->http_headers = value; break;
		case CURLOPT_POSTFIELDS:
			data->post_fields = value;
			free(data->post_fields_copy);
			data->post_fields_copy = NULL;
			data->mime = NULL;
			data->post = true;
			data->nobody = false;
			data->upload = false;
			break;
		case CURLOPT_COPYPOSTFIELDS:
		{
			free(data->post_fields_copy);
			data->post_fields_copy = NULL;
			if(value)
			{
				size_t length = data->post_field_size >= 0 ? (size_t)data->post_field_size : strlen(value);
				data->post_fields_copy = malloc(length + 1);
				if(!data->post_fields_copy) return CURLE_OUT_OF_MEMORY;
				memcpy(data->post_fields_copy, value, length);
				data->post_fields_copy[length] = '\0';
			}
			data->post_fields = data->post_fields_copy;
			data->mime = NULL;
			data->post = true;
			data->nobody = false;
			data->upload = false;
			break;
		}
		case CURLOPT_MIMEPOST:
			data->mime = value;
			data->post_fields = NULL;
			data->post = value != NULL;
			data->nobody = false;
			data->upload = false;
			break;
		case CURLOPT_WRITEDATA: data->write_data = value; break;
		case CURLOPT_HEADERDATA: data->header_data = value; break;
		case CURLOPT_READDATA: data->read_data = value; break;
		case CURLOPT_XFERINFODATA: data->xferinfo_data = value; data->progress_data = value; break;
		case CURLOPT_DEBUGDATA: data->debug_data = value; break;
		case CURLOPT_ERRORBUFFER: data->error_buffer = value; break;
		case CURLOPT_PRIVATE: data->private_data = value; break;
		default:
			break;
	}
	return CURLE_OK;
}

static CURLcode lcf_setopt_function(Curl_easy *data, CURLoption option, void *value)
{
	switch(option)
	{
		case CURLOPT_WRITEFUNCTION: data->write_cb = (curl_write_callback)value; break;
		case CURLOPT_HEADERFUNCTION: data->header_cb = (curl_write_callback)value; break;
		case CURLOPT_READFUNCTION: data->read_cb = (curl_read_callback)value; break;
		case CURLOPT_XFERINFOFUNCTION: data->xferinfo_cb = (curl_xferinfo_callback)value; break;
		case CURLOPT_PROGRESSFUNCTION: data->progress_cb = (curl_progress_callback)value; break;
		case CURLOPT_DEBUGFUNCTION: data->debug_cb = (curl_debug_callback)value; break;
		default:
			break;
	}
	return CURLE_OK;
}

static CURLcode lcf_setopt_off_t(Curl_easy *data, CURLoption option, curl_off_t value)
{
	switch(option)
	{
		case CURLOPT_INFILESIZE_LARGE: data->in_file_size = value; break;
		case CURLOPT_POSTFIELDSIZE_LARGE: data->post_field_size = value; break;
		default:
			break;
	}
	return CURLE_OK;
}

#undef curl_easy_setopt
CURL_EXTERN CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...)
{
	Curl_easy *data = curl;
	if(!data || data->magic != LCF_EASY_MAGIC) return CURLE_BAD_FUNCTION_ARGUMENT;

	va_list args;
	va_start(args, option);
	CURLcode result = CURLE_OK;

	if(option < CURLOPTTYPE_OBJECTPOINT)
	{
		result = lcf_setopt_long(data, option, va_arg(args, long));
	}
	else if(option < CURLOPTTYPE_FUNCTIONPOINT)
	{
		result = lcf_setopt_pointer(data, option, va_arg(args, void *));
	}
	else if(option < CURLOPTTYPE_OFF_T)
	{
		result = lcf_setopt_function(data, option, va_arg(args, void *));
	}
	else if(option < CURLOPTTYPE_BLOB)
	{
		result = lcf_setopt_off_t(data, option, va_arg(args, curl_off_t));
	}
	else
	{
		(void)va_arg(args, void *); /* Certificates and keys: not applicable. */
	}

	va_end(args);
	return result;
}

static struct curl_certinfo lcf_no_certificates = {0, NULL};

#undef curl_easy_getinfo
CURL_EXTERN CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...)
{
	Curl_easy *data = curl;
	if(!data || data->magic != LCF_EASY_MAGIC) return CURLE_BAD_FUNCTION_ARGUMENT;

	va_list args;
	va_start(args, info);
	void *out = va_arg(args, void *);
	va_end(args);
	if(!out) return CURLE_BAD_FUNCTION_ARGUMENT;

	const char *scheme = NULL;
	if(data->effective_url)
	{
		scheme = strncmp(data->effective_url, "https:", 6) == 0 ? "https" : "http";
	}

	switch(info & CURLINFO_TYPEMASK)
	{
		case CURLINFO_STRING:
		{
			char **value = out;
			switch(info)
			{
				case CURLINFO_EFFECTIVE_URL: *value = data->effective_url ? data->effective_url : data->url; break;
				case CURLINFO_CONTENT_TYPE: *value = data->content_type; break;
				case CURLINFO_PRIVATE: *value = data->private_data; break;
				case CURLINFO_REDIRECT_URL: *value = data->redirect_url; break;
				case CURLINFO_PRIMARY_IP: *value = ""; break;
				case CURLINFO_LOCAL_IP: *value = ""; break;
				case CURLINFO_SCHEME: *value = (char *)scheme; break;
				case CURLINFO_REFERER: *value = data->referer; break;
				default: *value = NULL; break;
			}
			return CURLE_OK;
		}
		case CURLINFO_LONG:
		{
			long *value = out;
			switch(info)
			{
				case CURLINFO_RESPONSE_CODE: *value = data->response_code; break;
				case CURLINFO_HEADER_SIZE: *value = data->header_size; break;
				case CURLINFO_REQUEST_SIZE: *value = data->request_size; break;
				case CURLINFO_REDIRECT_COUNT: *value = data->redirect_count; break;
				case CURLINFO_FILETIME: *value = -1; break;
				case CURLINFO_HTTP_VERSION: *value = data->http_version; break;
				case CURLINFO_NUM_CONNECTS: *value = data->response_code ? 1 : 0; break;
				case CURLINFO_PROTOCOL:
					*value = scheme ? (strcmp(scheme, "https") == 0 ? CURLPROTO_HTTPS : CURLPROTO_HTTP) : 0;
					break;
				case CURLINFO_PRIMARY_PORT:
					*value = scheme ? (strcmp(scheme, "https") == 0 ? 443 : 80) : 0;
					if(data->port) *value = data->port;
					break;
				default: *value = 0; break;
			}
			return CURLE_OK;
		}
		case CURLINFO_DOUBLE:
		{
			double *value = out;
			switch(info)
			{
				case CURLINFO_TOTAL_TIME: *value = data->total_time; break;
				case CURLINFO_STARTTRANSFER_TIME: *value = data->starttransfer_time; break;
				case CURLINFO_SIZE_DOWNLOAD: *value = (double)data->size_download; break;
				case CURLINFO_SIZE_UPLOAD: *value = (double)data->size_upload; break;
				case CURLINFO_SPEED_DOWNLOAD:
					*value = data->total_time > 0 ? data->size_download / data->total_time : 0;
					break;
				case CURLINFO_SPEED_UPLOAD:
					*value = data->total_time > 0 ? data->size_upload / data->total_time : 0;
					break;
				case CURLINFO_CONTENT_LENGTH_DOWNLOAD: *value = (double)data->content_length_download; break;
				case CURLINFO_CONTENT_LENGTH_UPLOAD: *value = (double)data->content_length_upload; break;
				default: *value = 0; break;
			}
			return CURLE_OK;
		}
		case CURLINFO_OFF_T:
		{
			curl_off_t *value = out;
			switch(info)
			{
				case CURLINFO_SIZE_DOWNLOAD_T: *value = data->size_download; break;
				case CURLINFO_SIZE_UPLOAD_T: *value = data->size_upload; break;
				case CURLINFO_SPEED_DOWNLOAD_T:
					*value = data->total_time > 0 ? (curl_off_t)(data->size_download / data->total_time) : 0;
					break;
				case CURLINFO_SPEED_UPLOAD_T:
					*value = data->total_time > 0 ? (curl_off_t)(data->size_upload / data->total_time) : 0;
					break;
				case CURLINFO_CONTENT_LENGTH_DOWNLOAD_T: *value = data->content_length_download; break;
				case CURLINFO_CONTENT_LENGTH_UPLOAD_T: *value = data->content_length_upload; break;
				case CURLINFO_TOTAL_TIME_T: *value = (curl_off_t)(data->total_time * 1000000); break;
				case CURLINFO_STARTTRANSFER_TIME_T: *value = (curl_off_t)(data->starttransfer_time * 1000000); break;
				case CURLINFO_FILETIME_T: *value = -1; break;
				default: *value = 0; break;
			}
			return CURLE_OK;
		}
		case CURLINFO_SLIST:
			if(info == CURLINFO_CERTINFO)
			{
				*(struct curl_certinfo **)out = &lcf_no_certificates;
			}
			else
			{
				*(void **)out = NULL;
			}
			return CURLE_OK;
		case CURLINFO_SOCKET:
			*(curl_socket_t *)out = CURL_SOCKET_BAD;
			return CURLE_OK;
	}

	return CURLE_UNKNOWN_OPTION;
}

CURL_EXTERN CURLcode curl_easy_pause(CURL *handle, int bitmask)
{
	(void)handle;
	(void)bitmask;
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_easy_upkeep(CURL *curl)
{
	(void)curl;
	return CURLE_OK;
}

static int lcf_unreserved(unsigned char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
		|| c == '-' || c == '.' || c == '_' || c == '~';
}

CURL_EXTERN char *curl_easy_escape(CURL *handle, const char *string, int length)
{
	(void)handle;
	if(!string) return NULL;
	size_t size = length > 0 ? (size_t)length : strlen(string);
	char *out = malloc(size * 3 + 1);
	if(!out) return NULL;
	size_t o = 0;
	for(size_t i = 0; i < size; i++)
	{
		unsigned char c = (unsigned char)string[i];
		if(lcf_unreserved(c))
		{
			out[o++] = c;
		}
		else
		{
			o += snprintf(out + o, 4, "%%%02X", c);
		}
	}
	out[o] = '\0';
	return out;
}

CURL_EXTERN char *curl_escape(const char *string, int length)
{
	return curl_easy_escape(NULL, string, length);
}

static int lcf_hex(char c)
{
	if(c >= '0' && c <= '9') return c - '0';
	if(c >= 'a' && c <= 'f') return c - 'a' + 10;
	if(c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

CURL_EXTERN char *curl_easy_unescape(CURL *handle, const char *string, int length, int *outlength)
{
	(void)handle;
	if(!string) return NULL;
	size_t size = length > 0 ? (size_t)length : strlen(string);
	char *out = malloc(size + 1);
	if(!out) return NULL;
	size_t o = 0;
	for(size_t i = 0; i < size; i++)
	{
		if(string[i] == '%' && i + 2 < size
			&& lcf_hex(string[i + 1]) >= 0 && lcf_hex(string[i + 2]) >= 0)
		{
			out[o++] = (char)(lcf_hex(string[i + 1]) * 16 + lcf_hex(string[i + 2]));
			i += 2;
		}
		else
		{
			out[o++] = string[i];
		}
	}
	out[o] = '\0';
	if(outlength) *outlength = (int)o;
	return out;
}

CURL_EXTERN char *curl_unescape(const char *string, int length)
{
	return curl_easy_unescape(NULL, string, length, NULL);
}

CURL_EXTERN struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data)
{
	struct curl_slist *item = malloc(sizeof(struct curl_slist));
	if(!item) return NULL;
	item->data = lcf_strdup(data);
	item->next = NULL;
	if(!item->data)
	{
		free(item);
		return NULL;
	}
	if(!list) return item;
	struct curl_slist *last = list;
	while(last->next) last = last->next;
	last->next = item;
	return list;
}

CURL_EXTERN void curl_slist_free_all(struct curl_slist *list)
{
	while(list)
	{
		struct curl_slist *next = list->next;
		free(list->data);
		free(list);
		list = next;
	}
}

CURL_EXTERN const char *curl_easy_strerror(CURLcode code)
{
	switch(code)
	{
		case CURLE_OK: return "No error";
		case CURLE_UNSUPPORTED_PROTOCOL: return "Unsupported protocol";
		case CURLE_FAILED_INIT: return "Failed initialization";
		case CURLE_URL_MALFORMAT: return "URL using bad/illegal format or missing URL";
		case CURLE_NOT_BUILT_IN: return "A requested feature, protocol or option was not found built-in in this libcurl due to a build-time decision.";
		case CURLE_COULDNT_RESOLVE_PROXY: return "Could not resolve proxy name";
		case CURLE_COULDNT_RESOLVE_HOST: return "Could not resolve hostname";
		case CURLE_COULDNT_CONNECT: return "Could not connect to server";
		case CURLE_HTTP_RETURNED_ERROR: return "HTTP response code said error";
		case CURLE_WRITE_ERROR: return "Failed writing received data to disk/application";
		case CURLE_READ_ERROR: return "Failed to open/read local data from file/application";
		case CURLE_OUT_OF_MEMORY: return "Out of memory";
		case CURLE_OPERATION_TIMEDOUT: return "Timeout was reached";
		case CURLE_BAD_FUNCTION_ARGUMENT: return "A libcurl function was given a bad argument";
		case CURLE_ABORTED_BY_CALLBACK: return "Operation was aborted by an application callback";
		case CURLE_TOO_MANY_REDIRECTS: return "Number of redirects hit maximum amount";
		case CURLE_UNKNOWN_OPTION: return "An unknown option was passed in to libcurl";
		case CURLE_GOT_NOTHING: return "Server returned nothing (no headers, no data)";
		case CURLE_SEND_ERROR: return "Failed sending data to the peer";
		case CURLE_RECV_ERROR: return "Failure when receiving data from the peer";
		default: return "Unknown error";
	}
}
