# php-wasm-curl

The libcurl C API implemented over `fetch()`, for Emscripten.

It exists so PHP's unchanged `ext/curl` can be compiled for
[php-wasm](https://github.com/seanmorris/php-wasm): PHP provides `curl_*`, and this
library turns each transfer's options into a `fetch()` call. It delivers headers
before reading the response body, then streams chunks through libcurl's write
and progress callbacks. Asyncify lets `curl_easy_perform()` remain synchronous
for PHP while JavaScript performs the network IO.

curl's public headers are vendored from curl 8.22.0 in `include/curl/`, so option,
info and error values match real libcurl.

## Supported

- Easy handles: `curl_easy_init`, `setopt`, `perform`, `getinfo`, `reset`,
  `duphandle`, `escape`, `unescape`, `strerror`.
- Multi handles: `curl_multi_perform()` starts pending transfers and delivers
  available events. `curl_multi_wait()`/`poll()` await new events; completed
  transfers are reported individually through `curl_multi_info_read()`.
- MIME (`curl_mime_*`) uploads, including callback-backed parts and binary fields.
- Share handles (accepted; nothing is shared), `curl_slist_*`, `curl_version_info`.
- Requests: method selection, headers (`Name:` removes, `Name;` sends empty),
  POST/PUT bodies from fields or a read callback, Basic, Digest, and bearer authentication,
  user agent, referer, cookie, range, port, redirects, timeouts, `FAILONERROR`.
- Results: status, headers, body, effective and redirect URLs, sizes and timings.
- Verbose request and response headers through `CURLOPT_DEBUGFUNCTION` or
  `CURLOPT_STDERR`.
- Streamed downloads with bounded transport buffering, early header callbacks,
  download progress, and cancellation during transfers.

In server runtimes, redirect limits are enforced before requesting the next URL.
Browsers hide intermediate redirect responses. Disabled redirects stop with an
explicit unsupported-feature error; a zero redirect limit returns
`CURLE_TOO_MANY_REDIRECTS`. When following is enabled, positive limits remain
subject to the browser's own redirect limit.

See [the compatibility guide](test/guzzle/README.md#compatibility-limits) for the
behavior that differs from native curl and browser credential configuration.

## Build

Requires Emscripten.

```sh
make                       # build/libcurl.a and build/libcurl.pc
make install PREFIX=/path  # lib/libcurl.a, lib/pkgconfig/libcurl.pc, include/curl
make check                 # C API tests under Node
```

Link the library into an Asyncify build (`-sASYNCIFY=1`). Side modules also need
`'-sASYNCIFY_IMPORTS=*'`.

`libcurl.pc` deliberately omits SSL from `supported_features`, so PHP's configure
skips a TLS-library probe that cannot run when cross-compiling; at runtime
`curl_version_info()` still reports SSL. PHP's configure also needs
`ac_cv_lib_curl_curl_easy_perform=yes`, because its `curl_easy_perform` link probe
fails in Binaryen when built with `-g`.

## Guzzle integration

`make check-guzzle PHP_SRC=/path/to/configured/php-8.4.1` compiles PHP's unchanged
`ext/curl` against this library and loads it into the published `php-wasm` Node
runtime. The suite runs Guzzle 7 with its default handler, `CurlHandler`, and
`CurlMultiHandler`, including HTTPS, uploads, cookies, redirects, error handling,
callbacks, and concurrent requests with `Pool`. `make check-browser` runs the
shared Guzzle suite in Chromium, Firefox, and WebKit using `PhpWeb`.

See [the integration test guide](test/guzzle/README.md) for dependencies and the
build recipe and browser configuration. Browser CORS, cookies, redirects, and
transport controls still differ from native curl.

## License

Dual licensed under the Apache License, Version 2.0 and the GNU General Public
License Version 2. See `NOTICE`. The vendored curl headers are under the curl
license (`COPYING.curl`).
