#include <stdlib.h>
#include <stdarg.h>
#include "internal.h"

#define LCF_VERSION LIBCURL_VERSION "-php-wasm-fetch"

static const char *const lcf_protocols[] = {"http", "https", NULL};
static const char *const lcf_feature_names[] = {"IPv6", "Largefile", "libz", "SSL", NULL};

static curl_version_info_data lcf_version_info = {
	.age = CURLVERSION_NOW,
	.version = LCF_VERSION,
	.version_num = LIBCURL_VERSION_NUM,
	.host = "wasm32-unknown-emscripten",
	.features = CURL_VERSION_IPV6 | CURL_VERSION_LARGEFILE | CURL_VERSION_LIBZ | CURL_VERSION_SSL,
	.ssl_version = "fetch",
	.libz_version = "fetch",
	.protocols = lcf_protocols,
	.feature_names = lcf_feature_names,
};

CURL_EXTERN curl_version_info_data *curl_version_info(CURLversion stamp)
{
	(void)stamp;
	return &lcf_version_info;
}

CURL_EXTERN char *curl_version(void)
{
	return "libcurl/" LCF_VERSION;
}

CURL_EXTERN CURLSH *curl_share_init(void)
{
	Curl_share *share = calloc(1, sizeof(Curl_share));
	if(share) share->magic = LCF_SHARE_MAGIC;
	return share;
}

#undef curl_share_setopt
CURL_EXTERN CURLSHcode curl_share_setopt(CURLSH *sh, CURLSHoption option, ...)
{
	(void)option;
	Curl_share *share = sh;
	return share && share->magic == LCF_SHARE_MAGIC ? CURLSHE_OK : CURLSHE_INVALID;
}

CURL_EXTERN CURLSHcode curl_share_cleanup(CURLSH *sh)
{
	Curl_share *share = sh;
	if(!share || share->magic != LCF_SHARE_MAGIC) return CURLSHE_INVALID;
	share->magic = 0;
	free(share);
	return CURLSHE_OK;
}

CURL_EXTERN const char *curl_share_strerror(CURLSHcode code)
{
	switch(code)
	{
		case CURLSHE_OK: return "No error";
		case CURLSHE_BAD_OPTION: return "Unknown share option";
		case CURLSHE_IN_USE: return "Share currently in use";
		case CURLSHE_INVALID: return "Invalid share handle";
		case CURLSHE_NOMEM: return "Out of memory";
		case CURLSHE_NOT_BUILT_IN: return "Feature not enabled in this library";
		default: return "CURLSHcode unknown";
	}
}
