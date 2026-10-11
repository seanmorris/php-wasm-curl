#include <stdlib.h>
#include <string.h>
#include "internal.h"

/* Fetch IO runs independently; perform pumps callbacks without waiting for IO.
   wait/poll yield to JS until one of this multi handle's events is ready. */

CURL_EXTERN CURLM *curl_multi_init(void)
{
	Curl_multi *multi = calloc(1, sizeof(Curl_multi));
	if(!multi) return NULL;
	multi->magic = LCF_MULTI_MAGIC;
	return multi;
}

static int lcf_valid_multi(Curl_multi *multi)
{
	return multi && multi->magic == LCF_MULTI_MAGIC;
}

static void lcf_drop_messages_for(Curl_multi *multi, Curl_easy *data)
{
	/* Removing a handle also ends the lifetime of previously returned messages. */
	size_t kept = 0;
	for(size_t i = multi->message_read; i < multi->message_count; i++)
	{
		if(multi->messages[i].easy_handle == data) continue;
		multi->messages[kept++] = multi->messages[i];
	}
	multi->message_count = kept;
	multi->message_read = 0;
	if(!kept)
	{
		free(multi->messages);
		multi->messages = NULL;
	}
}

CURL_EXTERN CURLMcode curl_multi_add_handle(CURLM *m, CURL *curl)
{
	Curl_multi *multi = m;
	Curl_easy *data = curl;
	if(!lcf_valid_multi(multi)) return CURLM_BAD_HANDLE;
	if(!data || data->magic != LCF_EASY_MAGIC) return CURLM_BAD_EASY_HANDLE;
	if(data->multi) return CURLM_ADDED_ALREADY;

	data->multi = multi;
	data->done = false;
	data->result = CURLE_OK;
	data->next_in_multi = NULL;

	if(!multi->first)
	{
		multi->first = data;
	}
	else
	{
		Curl_easy *last = multi->first;
		while(last->next_in_multi) last = last->next_in_multi;
		last->next_in_multi = data;
	}
	return CURLM_OK;
}

CURL_EXTERN CURLMcode curl_multi_remove_handle(CURLM *m, CURL *curl)
{
	Curl_multi *multi = m;
	Curl_easy *data = curl;
	if(!lcf_valid_multi(multi)) return CURLM_BAD_HANDLE;
	if(!data || data->magic != LCF_EASY_MAGIC) return CURLM_BAD_EASY_HANDLE;
	if(data->multi != multi) return CURLM_OK;

	Curl_easy **link = &multi->first;
	while(*link && *link != data) link = &(*link)->next_in_multi;
	if(*link) *link = data->next_in_multi;

	if(data->request_id)
	{
		lcf_js_request_free(data->request_id);
		data->request_id = 0;
	}
	lcf_drop_messages_for(multi, data);
	data->multi = NULL;
	data->next_in_multi = NULL;
	return CURLM_OK;
}

static int lcf_queue_done(Curl_multi *multi, Curl_easy *data)
{
	CURLMsg *messages = realloc(multi->messages, (multi->message_count + 1) * sizeof(CURLMsg));
	if(!messages) return 0;
	multi->messages = messages;
	multi->messages[multi->message_count].msg = CURLMSG_DONE;
	multi->messages[multi->message_count].easy_handle = data;
	multi->messages[multi->message_count].data.result = data->result;
	multi->message_count++;
	return 1;
}

CURL_EXTERN CURLMcode curl_multi_perform(CURLM *m, int *running_handles)
{
	Curl_multi *multi = m;
	if(!lcf_valid_multi(multi)) return CURLM_BAD_HANDLE;

	int running = 0;
	for(Curl_easy *data = multi->first; data; data = data->next_in_multi)
	{
		if(data->done) continue;
		if(!data->request_id)
		{
			CURLcode code = lcf_prepare(data);
			if(code != CURLE_OK)
			{
				data->done = true;
				data->result = code;
			}
			else lcf_js_start(data->request_id);
		}
		if(!data->done) lcf_pump(data);
		if(data->done)
		{
			if(!lcf_queue_done(multi, data)) return CURLM_OUT_OF_MEMORY;
		}
		else running++;
	}
	if(running_handles) *running_handles = running;
	return CURLM_OK;
}

CURL_EXTERN CURLMcode curl_multi_wait(CURLM *m, struct curl_waitfd extra_fds[], unsigned int extra_nfds, int timeout_ms, int *ret)
{
	(void)extra_fds;
	Curl_multi *multi = m;
	if(!lcf_valid_multi(multi)) return CURLM_BAD_HANDLE;
	if(timeout_ms < 0 || extra_nfds) return CURLM_BAD_FUNCTION_ARGUMENT;
	if(ret) *ret = 0;
	size_t count = 0;
	for(Curl_easy *data = multi->first; data; data = data->next_in_multi)
	{
		/* A newly added handle needs perform() before waiting on existing IO.
		   Guzzle can refill a pool between perform() and select(). */
		if(!data->done && !data->request_id) return CURLM_OK;
		if(data->request_id && !data->done) count++;
	}
	if(!count) return CURLM_OK;
	int *ids = malloc(count * sizeof(int));
	if(!ids) return CURLM_OUT_OF_MEMORY;
	count = 0;
	for(Curl_easy *data = multi->first; data; data = data->next_in_multi)
		if(data->request_id && !data->done) ids[count++] = data->request_id;
	int ready = lcf_js_wait(ids, (int)count, timeout_ms);
	free(ids);
	if(ret) *ret = ready;
	return CURLM_OK;
}

CURL_EXTERN CURLMcode curl_multi_poll(CURLM *m, struct curl_waitfd extra_fds[], unsigned int extra_nfds, int timeout_ms, int *ret)
{
	return curl_multi_wait(m, extra_fds, extra_nfds, timeout_ms, ret);
}

CURL_EXTERN CURLMcode curl_multi_wakeup(CURLM *m)
{
	return lcf_valid_multi(m) ? CURLM_OK : CURLM_BAD_HANDLE;
}

CURL_EXTERN CURLMsg *curl_multi_info_read(CURLM *m, int *msgs_in_queue)
{
	Curl_multi *multi = m;
	if(msgs_in_queue) *msgs_in_queue = 0;
	if(!lcf_valid_multi(multi) || multi->message_read >= multi->message_count) return NULL;

	CURLMsg *message = &multi->messages[multi->message_read++];
	if(msgs_in_queue) *msgs_in_queue = (int)(multi->message_count - multi->message_read);
	return message;
}

#undef curl_multi_setopt
CURL_EXTERN CURLMcode curl_multi_setopt(CURLM *m, CURLMoption option, ...)
{
	(void)option;
	return lcf_valid_multi(m) ? CURLM_OK : CURLM_BAD_HANDLE;
}

CURL_EXTERN CURLMcode curl_multi_cleanup(CURLM *m)
{
	Curl_multi *multi = m;
	if(!lcf_valid_multi(multi)) return CURLM_BAD_HANDLE;
	while(multi->first) curl_multi_remove_handle(multi, multi->first);
	free(multi->messages);
	multi->magic = 0;
	free(multi);
	return CURLM_OK;
}

CURL_EXTERN const char *curl_multi_strerror(CURLMcode code)
{
	switch(code)
	{
		case CURLM_CALL_MULTI_PERFORM: return "Please call curl_multi_perform() soon";
		case CURLM_OK: return "No error";
		case CURLM_BAD_HANDLE: return "Invalid multi handle";
		case CURLM_BAD_EASY_HANDLE: return "Invalid easy handle";
		case CURLM_OUT_OF_MEMORY: return "Out of memory";
		case CURLM_INTERNAL_ERROR: return "Internal error";
		case CURLM_UNKNOWN_OPTION: return "Unknown option";
		case CURLM_ADDED_ALREADY: return "The easy handle is already added to a multi handle";
		default: return "Unknown error";
	}
}
