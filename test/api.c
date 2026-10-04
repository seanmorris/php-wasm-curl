/*
 * Exercises the libcurl API against test/server.mjs. Built as a side module and
 * loaded with dlopen() from an Asyncify main module, as PHP loads extensions.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <curl/curl.h>

static int failures = 0;
static int number = 0;
static char base[128];

#define CHECK(condition, name) do { \
	number++; \
	if(condition) printf("ok %d - %s\n", number, name); \
	else { failures++; printf("not ok %d - %s (line %d)\n", number, name, __LINE__); } \
} while(0)

typedef struct {
	char data[16384];
	size_t length;
} sink;

static size_t to_sink(char *data, size_t size, size_t count, void *target)
{
	sink *out = target;
	size_t length = size * count;
	if(out->length + length >= sizeof(out->data)) return 0;
	memcpy(out->data + out->length, data, length);
	out->length += length;
	out->data[out->length] = '\0';
	return length;
}

static CURL *easy(const char *path, sink *body, sink *headers)
{
	char url[256];
	snprintf(url, sizeof(url), "%s%s", base, path);
	CURL *curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, url);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, to_sink);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, body);
	if(headers)
	{
		curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, to_sink);
		curl_easy_setopt(curl, CURLOPT_HEADERDATA, headers);
	}
	return curl;
}

static void test_get(void)
{
	sink body = {0}, headers = {0};
	CURL *curl = easy("/hello", &body, &headers);
	CURLcode code = curl_easy_perform(curl);
	long status = 0;
	char *type = NULL;
	curl_off_t downloaded = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &type);
	curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
	CHECK(code == CURLE_OK, "GET succeeds");
	CHECK(status == 200, "GET response code");
	CHECK(strcmp(body.data, "Hello, curl!") == 0, "GET body");
	CHECK(type && strcmp(type, "text/plain") == 0, "CURLINFO_CONTENT_TYPE");
	CHECK(downloaded == 12, "CURLINFO_SIZE_DOWNLOAD_T");
	CHECK(strncmp(headers.data, "HTTP/1.1 200", 12) == 0, "status line first");
	CHECK(strstr(headers.data, "x-test: yes\r\n") != NULL, "response header delivered");
	CHECK(headers.length >= 2 && strcmp(headers.data + headers.length - 4, "\r\n\r\n") == 0, "headers end with a blank line");
	curl_easy_cleanup(curl);
}

static void test_header_in_body(void)
{
	sink body = {0};
	CURL *curl = easy("/hello", &body, NULL);
	curl_easy_setopt(curl, CURLOPT_HEADER, 1L);
	curl_easy_perform(curl);
	CHECK(strncmp(body.data, "HTTP/1.1 200", 12) == 0 && strstr(body.data, "\r\n\r\nHello, curl!"), "CURLOPT_HEADER prefixes the body");
	curl_easy_cleanup(curl);
}

static void test_post(void)
{
	sink body = {0};
	CURL *curl = easy("/echo", &body, NULL);
	struct curl_slist *list = NULL;
	list = curl_slist_append(list, "X-Custom: one");
	list = curl_slist_append(list, "Expect:");
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, 7L);
	curl_easy_setopt(curl, CURLOPT_COPYPOSTFIELDS, "a=1&b=2");
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "lcf-test");
	CURLcode code = curl_easy_perform(curl);
	CHECK(code == CURLE_OK, "POST succeeds");
	CHECK(strstr(body.data, "\"method\":\"POST\"") != NULL, "POST method");
	CHECK(strstr(body.data, "\"body\":\"a=1&b=2\"") != NULL, "POST body");
	CHECK(strstr(body.data, "\"x-custom\":\"one\"") != NULL, "custom header sent");
	CHECK(strstr(body.data, "application/x-www-form-urlencoded") != NULL, "default form content type");
	CHECK(strstr(body.data, "\"user-agent\":\"lcf-test\"") != NULL, "CURLOPT_USERAGENT");
	curl_slist_free_all(list);
	curl_easy_cleanup(curl);
}

static char upload_data[] = "uploaded via read callback";
static size_t upload_offset = 0;

static size_t from_upload(char *buffer, size_t size, size_t count, void *arg)
{
	(void)arg;
	size_t left = strlen(upload_data) - upload_offset;
	size_t length = size * count < left ? size * count : left;
	memcpy(buffer, upload_data + upload_offset, length);
	upload_offset += length;
	return length;
}

static void test_upload(void)
{
	sink body = {0};
	CURL *curl = easy("/echo", &body, NULL);
	curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
	curl_easy_setopt(curl, CURLOPT_READFUNCTION, from_upload);
	curl_easy_setopt(curl, CURLOPT_INFILESIZE, (long)strlen(upload_data));
	CURLcode code = curl_easy_perform(curl);
	CHECK(code == CURLE_OK && strstr(body.data, "\"method\":\"PUT\"") && strstr(body.data, "uploaded via read callback"), "PUT through CURLOPT_READFUNCTION");
	curl_easy_cleanup(curl);
}

static void test_custom_and_auth(void)
{
	sink body = {0};
	CURL *curl = easy("/echo", &body, NULL);
	curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
	curl_easy_setopt(curl, CURLOPT_USERPWD, "user:secret");
	curl_easy_perform(curl);
	CHECK(strstr(body.data, "\"method\":\"DELETE\"") != NULL, "CURLOPT_CUSTOMREQUEST");
	CHECK(strstr(body.data, "Basic dXNlcjpzZWNyZXQ=") != NULL, "CURLOPT_USERPWD basic auth");
	curl_easy_cleanup(curl);
}

static void test_redirects(void)
{
	sink body = {0};
	CURL *curl = easy("/redirect", &body, NULL);
	curl_easy_perform(curl);
	long status = 0;
	char *location = NULL;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &location);
	CHECK(status == 302, "redirect is not followed by default");
	CHECK(location && strstr(location, "/final"), "CURLINFO_REDIRECT_URL is absolute");

	sink followed = {0};
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, &followed);
	curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	curl_easy_perform(curl);
	char *effective = NULL;
	long count = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
	curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective);
	curl_easy_getinfo(curl, CURLINFO_REDIRECT_COUNT, &count);
	CHECK(status == 200 && strcmp(followed.data, "final") == 0, "CURLOPT_FOLLOWLOCATION");
	CHECK(effective && strstr(effective, "/final"), "CURLINFO_EFFECTIVE_URL after redirect");
	CHECK(count == 1, "CURLINFO_REDIRECT_COUNT");
	curl_easy_cleanup(curl);
}

static void test_errors(void)
{
	char error[CURL_ERROR_SIZE] = "";
	sink body = {0};
	CURL *curl = easy("/missing", &body, NULL);
	curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	CURLcode code = curl_easy_perform(curl);
	CHECK(code == CURLE_HTTP_RETURNED_ERROR, "CURLOPT_FAILONERROR gives CURLE_HTTP_RETURNED_ERROR");
	CHECK(strstr(error, "404") != NULL, "error buffer names the status");
	CHECK(body.length == 0, "no body after FAILONERROR");
	curl_easy_cleanup(curl);

	curl = easy("/hang", &body, NULL);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 200L);
	code = curl_easy_perform(curl);
	CHECK(code == CURLE_OPERATION_TIMEDOUT, "CURLOPT_TIMEOUT_MS gives CURLE_OPERATION_TIMEDOUT");
	curl_easy_cleanup(curl);

	curl = curl_easy_init();
	curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:1/");
	curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error);
	code = curl_easy_perform(curl);
	CHECK(code == CURLE_COULDNT_CONNECT, "refused connection gives CURLE_COULDNT_CONNECT");
	CHECK(strlen(error) > 0, "connection error message");

	curl_easy_setopt(curl, CURLOPT_URL, "ftp://example.com/");
	code = curl_easy_perform(curl);
	CHECK(code == CURLE_UNSUPPORTED_PROTOCOL, "non-HTTP scheme is rejected");

	curl_easy_setopt(curl, CURLOPT_URL, "http://example.com/");
	curl_easy_setopt(curl, CURLOPT_PROXY, "http://proxy:8080");
	code = curl_easy_perform(curl);
	CHECK(code == CURLE_COULDNT_RESOLVE_PROXY, "proxies are refused");
	curl_easy_cleanup(curl);
}

static char file_contents[] = "file body";
static size_t file_offset = 0;
static int file_freed = 0;

static size_t file_read(char *buffer, size_t size, size_t count, void *arg)
{
	(void)arg;
	size_t left = strlen(file_contents) - file_offset;
	size_t length = size * count < left ? size * count : left;
	memcpy(buffer, file_contents + file_offset, length);
	file_offset += length;
	return length;
}

static int file_seek(void *arg, curl_off_t offset, int origin)
{
	(void)arg;
	(void)origin;
	file_offset = (size_t)offset;
	return CURL_SEEKFUNC_OK;
}

static void file_free(void *arg)
{
	(void)arg;
	file_freed = 1;
}

static void test_mime(void)
{
	sink body = {0};
	CURL *curl = easy("/echo", &body, NULL);
	curl_mime *mime = curl_mime_init(curl);
	curl_mimepart *part = curl_mime_addpart(mime);
	curl_mime_name(part, "field");
	curl_mime_data(part, "value", CURL_ZERO_TERMINATED);
	part = curl_mime_addpart(mime);
	curl_mime_name(part, "upload");
	curl_mime_filename(part, "notes.txt");
	curl_mime_type(part, "text/plain");
	curl_mime_data_cb(part, -1, file_read, file_seek, file_free, NULL);
	curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
	CURLcode code = curl_easy_perform(curl);
	CHECK(code == CURLE_OK, "multipart POST succeeds");
	CHECK(strstr(body.data, "multipart/form-data; boundary=") != NULL, "multipart content type");
	CHECK(strstr(body.data, "name=\\\"field\\\"\\r\\n\\r\\nvalue") != NULL, "form field sent");
	CHECK(strstr(body.data, "filename=\\\"notes.txt\\\"") && strstr(body.data, "file body"), "file part read through the callback");
	curl_mime_free(mime);
	CHECK(file_freed == 1, "curl_mime_free calls the free callback");
	curl_easy_cleanup(curl);
}

static void test_multi(void)
{
	CURLM *multi = curl_multi_init();
	CURL *handles[3];
	sink bodies[3] = {{{0}}};
	char path[64];
	for(int i = 0; i < 3; i++)
	{
		snprintf(path, sizeof(path), "/slow?ms=300&id=%d", i);
		handles[i] = easy(path, &bodies[i], NULL);
		curl_multi_add_handle(multi, handles[i]);
	}

	int running = 1, rounds = 0;
	while(running && rounds++ < 10)
	{
		curl_multi_perform(multi, &running);
		if(running) curl_multi_wait(multi, NULL, 0, 1000, NULL);
	}

	int done = 0, queued = 0;
	CURLMsg *message;
	while((message = curl_multi_info_read(multi, &queued)))
	{
		if(message->msg == CURLMSG_DONE && message->data.result == CURLE_OK) done++;
	}
	CHECK(done == 3, "three transfers finish with CURLMSG_DONE");
	CHECK(strcmp(bodies[0].data, "0") == 0 && strcmp(bodies[2].data, "2") == 0, "each handle gets its own body");

	sink peak = {0};
	CURL *probe = easy("/peak", &peak, NULL);
	curl_easy_perform(probe);
	CHECK(atoi(peak.data) == 3, "multi transfers overlap");
	curl_easy_cleanup(probe);

	for(int i = 0; i < 3; i++)
	{
		curl_multi_remove_handle(multi, handles[i]);
		curl_easy_cleanup(handles[i]);
	}
	curl_multi_cleanup(multi);
}

static void test_misc(void)
{
	char *escaped = curl_easy_escape(NULL, "a b&c/ü", 0);
	CHECK(strcmp(escaped, "a%20b%26c%2F%C3%BC") == 0, "curl_easy_escape");
	int length = 0;
	char *plain = curl_easy_unescape(NULL, escaped, 0, &length);
	CHECK(strcmp(plain, "a b&c/ü") == 0 && length == 8, "curl_easy_unescape");
	curl_free(escaped);
	curl_free(plain);

	curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
	CHECK(strstr(info->version, "php-wasm-fetch") && (info->features & CURL_VERSION_SSL), "curl_version_info");

	sink a = {0}, b = {0};
	CURL *original = easy("/hello", &a, NULL);
	CURL *copy = curl_easy_duphandle(original);
	curl_easy_setopt(copy, CURLOPT_WRITEDATA, &b);
	curl_easy_cleanup(original);
	CHECK(curl_easy_perform(copy) == CURLE_OK && strcmp(b.data, "Hello, curl!") == 0, "curl_easy_duphandle");
	curl_easy_cleanup(copy);
}

int run_tests(int port)
{
	snprintf(base, sizeof(base), "http://127.0.0.1:%d", port);
	curl_global_init(CURL_GLOBAL_DEFAULT);
	test_get();
	test_header_in_body();
	test_post();
	test_upload();
	test_custom_and_auth();
	test_redirects();
	test_errors();
	test_mime();
	test_multi();
	test_misc();
	curl_global_cleanup();
	printf("1..%d\n", number);
	return failures;
}
