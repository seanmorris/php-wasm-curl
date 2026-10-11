#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
php_source=${1:-}
output=${2:-"$repo_root/test/build/php8.4-curl.so"}
if [[ ! -f "$php_source/main/php_config.h" ]]; then
	printf '%s\n' 'Set PHP_SRC to an Emscripten-configured PHP 8.4 source tree. See test/guzzle/README.md.' >&2
	exit 1
fi
php_source=$(cd "$php_source" && pwd)
version=$(sed -n 's/^#define PHP_MAJOR_VERSION //p' "$php_source/main/php_version.h")
minor=$(sed -n 's/^#define PHP_MINOR_VERSION //p' "$php_source/main/php_version.h")
pointer_size=$(sed -n 's/^#define SIZEOF_SIZE_T //p' "$php_source/main/php_config.h")
if [[ "$version.$minor" != 8.4 || "$pointer_size" != 4 ]]; then
	printf '%s\n' 'The integration runtime requires PHP 8.4 headers configured for wasm32.' >&2
	exit 1
fi

mkdir -p "$(dirname "$output")"
emcc -O2 -fPIC -flto -DCOMPILE_DL_CURL=1 -DHAVE_CURL=1 -DCURL_DISABLE_TYPECHECK \
	-I"$repo_root/include" -I"$php_source" -I"$php_source/main" \
	-I"$php_source/Zend" -I"$php_source/TSRM" -I"$php_source/ext" -I"$php_source/ext/date/lib" \
	-sSIDE_MODULE=1 -sASYNCIFY=1 '-sASYNCIFY_IMPORTS=*' \
	"$php_source/ext/curl/interface.c" "$php_source/ext/curl/multi.c" \
	"$php_source/ext/curl/share.c" "$php_source/ext/curl/curl_file.c" \
	-Wl,--whole-archive "$repo_root/build/libcurl.a" -Wl,--no-whole-archive \
	-o "$output"
