# php-wasm-curl

The libcurl C API implemented over `fetch()`, for Emscripten.

It exists so PHP's unchanged `ext/curl` can be compiled for
[php-wasm](https://github.com/seanmorris/php-wasm): PHP provides `curl_*`, and this
library turns each transfer's options into a `fetch()` call, then replays the
response through libcurl's header, write and progress callbacks. The `fetch()` call
is awaited with Asyncify, so `curl_easy_perform()` stays synchronous for its caller.

curl's public headers are vendored from curl 8.22.0 in `include/curl/`, so option,
info and error values match real libcurl.

## Supported

- Easy handles: `curl_easy_init`, `setopt`, `perform`, `getinfo`, `reset`,
  `duphandle`, `escape`, `unescape`, `strerror`.
- Multi handles: every pending transfer starts together in `curl_multi_perform()`;
  results are reported through `curl_multi_info_read()`.
- MIME (`curl_mime_*`) uploads, including callback-backed parts, sent as `FormData`.
- Share handles (accepted; nothing is shared), `curl_slist_*`, `curl_version_info`.
- Requests: method selection, headers (`Name:` removes, `Name;` sends empty),
  POST/PUT bodies from fields or a read callback, basic and bearer authentication,
  user agent, referer, cookie, range, port, redirects, timeouts, `FAILONERROR`.
- Results: status, headers, body, effective and redirect URLs, sizes and timings.

See [php-wasm-curl's package README](https://github.com/seanmorris/php-wasm/tree/master/packages/curl#differences-from-native-curl)
for the behaviour that differs from native curl.

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

## License

Dual licensed under the Apache License, Version 2.0 and the GNU General Public
License Version 2. See `NOTICE`. The vendored curl headers are under the curl
license (`COPYING.curl`).
