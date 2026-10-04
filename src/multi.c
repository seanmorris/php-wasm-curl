#include <stdlib.h>
#include <string.h>
#include "internal.h"

/*
 * A multi handle runs every pending transfer together: curl_multi_perform()
 * starts their fetches concurrently, waits for all of them, then replays each
 * response through its easy handle's callbacks. It always returns with no
 * transfers running, so curl_multi_wait() never has to block.
 */

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
	size_t kept = multi->message_read;
	for(size_t i = multi->message_read; i < multi->message_count; i++)
	{
		if(multi->messages[i].easy_handle == data) continue;
		multi->messages[kept++] = multi->messages[i];
	}
	multi->message_count = kept;
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

	size_t pending = 0;
	for(Curl_easy *data = multi->first; data; data = data->next_in_multi)
	{
		if(!data->done) pending++;
	}

	if(pending)
	{
		int *ids = malloc(pending * sizeof(int));
		Curl_easy **handles = malloc(pending * sizeof(Curl_easy *));
		if(!ids || !handles)
		{
			free(ids);
			free(handles);
			return CURLM_OUT_OF_MEMORY;
		}

		size_t count = 0;
		for(Curl_easy *data = multi->first; data; data = data->next_in_multi)
		{
			if(data->done) continue;
			CURLcode code = lcf_prepare(data);
			if(code != CURLE_OK)
			{
				data->done = true;
				data->result = code;
				lcf_queue_done(multi, data);
				continue;
			}
			ids[count] = data->request_id;
			handles[count] = data;
			count++;
		}

		if(count) lcf_js_fetch_all(ids, (int)count);

		for(size_t i = 0; i < count; i++)
		{
			Curl_easy *data = handles[i];
			lcf_result result;
			lcf_take(data, &result);
			data->result = lcf_finish(data, &result);
			lcf_free_result(&result);
			data->done = true;
			lcf_queue_done(multi, data);
		}

		free(ids);
		free(handles);
	}

	if(running_handles) *running_handles = 0;
	return CURLM_OK;
}

CURL_EXTERN CURLMcode curl_multi_wait(CURLM *m, struct curl_waitfd extra_fds[], unsigned int extra_nfds, int timeout_ms, int *ret)
{
	(void)extra_fds;
	(void)extra_nfds;
	(void)timeout_ms;
	if(!lcf_valid_multi(m)) return CURLM_BAD_HANDLE;
	if(ret) *ret = 0;
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
