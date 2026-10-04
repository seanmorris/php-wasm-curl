#include <stdlib.h>
#include <string.h>
#include "internal.h"

CURL_EXTERN curl_mime *curl_mime_init(CURL *easy)
{
	(void)easy;
	return calloc(1, sizeof(curl_mime));
}

static void lcf_part_release_data(curl_mimepart *part)
{
	free(part->data);
	part->data = NULL;
	part->length = 0;
	if(part->free_cb) part->free_cb(part->cb_arg);
	part->read_cb = NULL;
	part->seek_cb = NULL;
	part->free_cb = NULL;
	part->cb_arg = NULL;
}

CURL_EXTERN void curl_mime_free(curl_mime *mime)
{
	if(!mime) return;
	curl_mimepart *part = mime->first;
	while(part)
	{
		curl_mimepart *next = part->next;
		lcf_part_release_data(part);
		free(part->name);
		free(part->filename);
		free(part->type);
		free(part);
		part = next;
	}
	free(mime);
}

CURL_EXTERN curl_mimepart *curl_mime_addpart(curl_mime *mime)
{
	if(!mime) return NULL;
	curl_mimepart *part = calloc(1, sizeof(curl_mimepart));
	if(!part) return NULL;
	part->cb_size = -1;
	if(mime->last) mime->last->next = part;
	else mime->first = part;
	mime->last = part;
	return part;
}

CURL_EXTERN CURLcode curl_mime_name(curl_mimepart *part, const char *name)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	lcf_set_string(&part->name, name);
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_filename(curl_mimepart *part, const char *filename)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	lcf_set_string(&part->filename, filename);
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_type(curl_mimepart *part, const char *mimetype)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	lcf_set_string(&part->type, mimetype);
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_data(curl_mimepart *part, const char *data, size_t datasize)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	lcf_part_release_data(part);
	if(!data) return CURLE_OK;
	if(datasize == CURL_ZERO_TERMINATED) datasize = strlen(data);
	part->data = malloc(datasize + 1);
	if(!part->data) return CURLE_OUT_OF_MEMORY;
	memcpy(part->data, data, datasize);
	part->data[datasize] = '\0';
	part->length = datasize;
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_data_cb(curl_mimepart *part, curl_off_t datasize,
	curl_read_callback readfunc, curl_seek_callback seekfunc, curl_free_callback freefunc, void *arg)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	lcf_part_release_data(part);
	part->cb_size = datasize;
	part->read_cb = readfunc;
	part->seek_cb = seekfunc;
	part->free_cb = freefunc;
	part->cb_arg = arg;
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_filedata(curl_mimepart *part, const char *filename)
{
	(void)part;
	(void)filename;
	return CURLE_NOT_BUILT_IN;
}

CURL_EXTERN CURLcode curl_mime_encoder(curl_mimepart *part, const char *encoding)
{
	(void)encoding;
	return part ? CURLE_OK : CURLE_BAD_FUNCTION_ARGUMENT;
}

CURL_EXTERN CURLcode curl_mime_headers(curl_mimepart *part, struct curl_slist *headers, int take_ownership)
{
	if(!part) return CURLE_BAD_FUNCTION_ARGUMENT;
	if(take_ownership) curl_slist_free_all(headers);
	return CURLE_OK;
}

CURL_EXTERN CURLcode curl_mime_subparts(curl_mimepart *part, curl_mime *subparts)
{
	(void)part;
	curl_mime_free(subparts);
	return CURLE_NOT_BUILT_IN;
}
