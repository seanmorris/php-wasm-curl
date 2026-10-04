#ifndef LIBCURL_FETCH_INTERNAL_H
#define LIBCURL_FETCH_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <curl/curl.h>

#define LCF_EASY_MAGIC  0x4c434645u /* "LCFE" */
#define LCF_MULTI_MAGIC 0x4c43464du /* "LCFM" */
#define LCF_SHARE_MAGIC 0x4c434653u /* "LCFS" */

typedef struct lcf_buffer {
	char *data;
	size_t length;
	size_t capacity;
} lcf_buffer;

/* One mime part. Callback-backed parts are read into data at perform time. */
struct curl_mimepart {
	struct curl_mimepart *next;
	char *name;
	char *filename;
	char *type;
	char *data;
	size_t length;
	curl_off_t cb_size;
	curl_read_callback read_cb;
	curl_seek_callback seek_cb;
	curl_free_callback free_cb;
	void *cb_arg;
};

struct curl_mime {
	struct curl_mimepart *first;
	struct curl_mimepart *last;
};

/* What a finished request reports back from JavaScript. */
typedef struct lcf_result {
	CURLcode code;
	long status;
	long redirected;
	char *status_text;
	char *url;
	char *headers;     /* "Name: value\r\n" lines, without the status line. */
	char *location;
	char *error;
	char *body;
	size_t body_length;
	double started;
	double headers_at;
	double finished;
} lcf_result;

typedef struct Curl_easy {
	unsigned magic;

	/* Request options. Strings are owned copies. */
	char *url;
	char *custom_request;
	char *user_agent;
	char *referer;
	char *cookie;
	char *range;
	char *username;
	char *password;
	char *bearer;
	char *proxy;
	char *unix_socket;
	long port;
	long http_auth;
	struct curl_slist *http_headers; /* Owned by the caller, as in libcurl. */

	bool post;
	bool nobody;
	bool upload;
	bool header_in_body;
	bool follow_location;
	bool fail_on_error;
	bool verbose;
	bool no_progress;
	long max_redirs;
	long timeout_ms;

	const char *post_fields;         /* Borrowed (CURLOPT_POSTFIELDS). */
	char *post_fields_copy;          /* Owned (CURLOPT_COPYPOSTFIELDS). */
	curl_off_t post_field_size;
	curl_off_t in_file_size;
	curl_mime *mime;                 /* Borrowed. */

	curl_write_callback write_cb;
	void *write_data;
	curl_write_callback header_cb;
	void *header_data;
	curl_read_callback read_cb;
	void *read_data;
	curl_xferinfo_callback xferinfo_cb;
	void *xferinfo_data;
	curl_progress_callback progress_cb;
	void *progress_data;
	curl_debug_callback debug_cb;
	void *debug_data;
	char *error_buffer;              /* Borrowed, CURL_ERROR_SIZE bytes. */
	void *private_data;

	/* Results of the last transfer. */
	long response_code;
	long http_version;
	long redirect_count;
	long header_size;
	long request_size;
	char *effective_url;
	char *content_type;
	char *redirect_url;
	curl_off_t size_download;
	curl_off_t size_upload;
	curl_off_t content_length_download;
	curl_off_t content_length_upload;
	double total_time;
	double starttransfer_time;

	/* Multi bookkeeping. */
	struct Curl_multi *multi;
	bool done;
	CURLcode result;
	struct Curl_easy *next_in_multi;
	int request_id;
} Curl_easy;

typedef struct Curl_multi {
	unsigned magic;
	Curl_easy *first;
	CURLMsg *messages;
	size_t message_count;
	size_t message_read;
} Curl_multi;

typedef struct Curl_share {
	unsigned magic;
} Curl_share;

/* easy.c */
char *lcf_strdup(const char *value);
void lcf_set_string(char **slot, const char *value);
void lcf_clear_results(Curl_easy *data);
void lcf_set_error(Curl_easy *data, CURLcode code, const char *message);

/* perform.c */
CURLcode lcf_prepare(Curl_easy *data);
CURLcode lcf_finish(Curl_easy *data, lcf_result *result);
void lcf_free_result(lcf_result *result);
CURLcode lcf_take(Curl_easy *data, lcf_result *result);

/* fetch_js.c */
extern int lcf_js_anchor;
int lcf_js_request_new(void);
void lcf_js_request_free(int id);
void lcf_js_request_url(int id, const char *url);
void lcf_js_request_method(int id, const char *method);
void lcf_js_request_header(int id, const char *name, const char *value);
void lcf_js_request_body(int id, const char *body, size_t length);
void lcf_js_request_form_field(int id, const char *name, const char *data, size_t length);
void lcf_js_request_form_file(int id, const char *name, const char *data, size_t length, const char *filename, const char *type);
void lcf_js_request_options(int id, int follow, long max_redirs, long timeout_ms, long port);
void lcf_js_fetch_all(const int *ids, int count);
void lcf_js_take_result(
	int id, int *code, long *status, long *redirected, char **status_text, char **url,
	char **headers, char **location, char **error, char **body, size_t *body_length,
	double *started, double *headers_at, double *finished
);

/* buffer helpers */
int lcf_buffer_append(lcf_buffer *buffer, const char *data, size_t length);
void lcf_buffer_free(lcf_buffer *buffer);

#endif
