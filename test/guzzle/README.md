# Guzzle on php-wasm

These tests run real Composer-installed Guzzle through PHP's unchanged `ext/curl`,
compiled against this repository's `libcurl.a`. They load the extension as a side
module into the published `PhpNode` and `PhpWeb` runtimes. Neither Guzzle nor PHP's curl
extension needs a patch or a custom handler.

The lockfiles pin `php-wasm` 0.2.0 (PHP 8.4.1) and Guzzle 7.15.5. The suite has been
tested with Node 22.23.2, Emscripten 6.0.12, and Playwright's Chromium, Firefox,
and WebKit engines.

## Run

Install Emscripten and activate its environment. You also need Node 22 or newer,
host PHP with Composer, OpenSSL, and a PHP 8.4.1 source release. From the repository
root, install the locked test dependencies:

```sh
npm ci --prefix test/guzzle
composer install --working-dir=test/guzzle --no-dev
```

Use a PHP source tree already configured for wasm32, or extract the
[official PHP 8.4.1 source release](https://www.php.net/distributions/php-8.4.1.tar.xz)
and run this inside it with Emscripten on `PATH`:

```sh
emconfigure ./configure \
  --disable-all --enable-embed=static --disable-cli --disable-cgi \
  --without-pcre-jit --disable-fiber-asm --disable-opcache-jit --without-pear
```

This generates the PHP headers; it does not build another PHP runtime. Return to
this repository and run:

```sh
make check check-guzzle PHP_SRC=/absolute/path/to/php-8.4.1
```

The integration target builds `test/build/php8.4-curl.so`, copies the Composer
vendor tree into php-wasm's filesystem, and runs `test.php` inside PHP. It starts
local HTTP and HTTPS fixtures on ephemeral ports. OpenSSL creates a temporary test
certificate, which is trusted through `NODE_EXTRA_CA_CERTS` for the test process;
certificate verification stays enabled. The certificate and key are deleted when
the runner exits. Requests do not need an external service.

After building the extension, `npm test --prefix test/guzzle` reruns just the PHP
suite. `PHP_CURL_EXTENSION=/absolute/path/to/curl.so` selects another build; the
suite checks that it is using this fetch-backed libcurl. A failing PHP assertion
or an incomplete run makes the runner exit unsuccessfully, including when the
embedded SAPI returns zero from PHP's `exit(1)`.

## Browser tests

After installing dependencies and building the PHP extension above:

```sh
cd test/guzzle
npx playwright install --with-deps chromium firefox webkit
cd ../..
make check-browser PHP_SRC=/absolute/path/to/php-8.4.1
```

`BROWSERS=chromium make check-browser` selects one engine. The default runs all
three. Each engine runs the shared PHP/Guzzle cases in four fresh browser
contexts: same-origin and cross-origin with the default credential policy,
cross-origin with credentials included, and same-origin with credentials omitted.
The runner uses real network fixtures and normal browser security settings.
It checks that requests and event waiters have been released after each suite.

The browser fixtures cover allowed and denied CORS preflights, exposed headers,
browser-owned cookies, disabled redirects, zero redirect limits, automatic
redirect following, and compressed responses whose encoding header is hidden by
CORS. The browser target uses `PhpWeb`; the pinned npm package does not include
the PHP worker binaries referenced by its `PhpWorker` wrapper.

## Browser configuration

Fetch defaults to same-origin browser credentials. Applications can explicitly
choose a different policy when creating their php-wasm runtime:

```js
const php = new PhpWeb({
  version: '8.4',
  sharedLibs: ['/path/to/php8.4-curl.so'],
  libcurlFetchOptions: {credentials: 'include'} // or 'omit', 'same-origin'
});
```

This controls the browser's cookie store, independently of Guzzle's PHP cookie
jar. Cross-origin credentialed requests still need the server's CORS permission,
and browser cookie policies still apply. The default remains `same-origin`.

Guzzle normally follows redirects in PHP middleware. Browsers cannot expose the
302 response to that middleware, so a redirect with curl following disabled now
fails explicitly without contacting its destination. For APIs that require
automatic browser redirects, use curl's transport following explicitly:

```php
$client = new GuzzleHttp\Client([
    // Use a curl handler directly; Fetch performs redirect following.
    'handler' => new GuzzleHttp\Handler\CurlHandler(),
    'curl' => [CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => -1],
]);
```

This example intentionally omits Guzzle's middleware stack. It does not provide
Guzzle redirect history, per-hop hooks, or a configurable redirect count. Fetch
still enforces its own redirect limit. Ordinary non-redirecting requests work
with Guzzle's default handler and middleware.

## Coverage

- Default Guzzle handler selection, explicit easy and multi handlers, and handle
  reuse across GET, JSON, form, and HEAD requests.
- HTTPS with runtime certificate verification; repeated and empty request
  headers; Basic and Digest authentication (MD5/SHA-256, session variants,
  `auth`/`auth-int`, stale nonces, invalid credentials, and hash block boundaries).
- Binary bodies, large callback-backed uploads, unknown-length streams, and
  multipart file uploads.
- Cookie jars and separate `Set-Cookie` headers, including redirect cookies.
- Redirect history, disabling redirects, redirect limits, and PSR-18 responses.
- HTTP errors, timeouts, connection errors, and curl error context.
- Early `on_headers`, `on_stats`, incremental download `progress`, callback
  exceptions that stop network reads, file sinks, debug streams, and decompressed
  response headers.
- Overlapping async requests, bounded `Pool` concurrency across several batches,
  prompt pool refills while slow requests remain active, cancellation before and
  during execution, cleanup of active transfers, and mixed completion states.
- Query encoding, DELETE, delayed requests, disabled HTTP exceptions, and retry
  middleware after server errors.
- The C suite also checks a 4 MiB download reaches callbacks without buffering
  the entire response in the WASM heap.

## Compatibility limits

The shared suite validates Guzzle's curl handlers in Node and browsers. Fetch
still owns the transport:

- Upload bodies are buffered before Fetch starts. Portable Fetch does not expose
  upload progress; upload counts are available once response headers arrive.
  Download chunks reach `sink` and progress callbacks incrementally, with at most
  one queued chunk per transfer in the shim. The sink itself can still retain
  the complete response, including files in php-wasm's in-memory filesystem.
  Guzzle's `stream` option can select PHP's separate stream handler; this library
  implements the curl handler, not that stream wrapper or its `read_timeout`.
- Browsers enforce CORS and restrict request and response headers. They hide
  `Set-Cookie` and intermediate redirects. Guzzle cannot reliably manage browser
  cookies or inspect redirect responses. Redirects can be stopped, but their
  status and headers cannot be returned as a normal Guzzle response. Positive
  curl redirect limits, per-hop tracking, and middleware policy cannot be
  implemented over automatic browser redirects.
- TLS verification and trust configuration belong to the JavaScript runtime.
  Guzzle's `verify`, client certificate, cipher, and other curl TLS options do not
  configure Fetch. This includes `verify => false`; it does not disable runtime
  verification. Configure Node's trust store outside PHP when needed.
- Proxies and Unix sockets are rejected. NTLM and direct connection, DNS, HTTP
  version, and connection-pool controls are not
  implemented; `connect_timeout` has no independent effect. `timeout` applies to
  the entire Fetch operation.
- Digest needs a readable `WWW-Authenticate` challenge; cross-origin servers must
  expose that header. SHA-256 uses Web Crypto and requires a secure browser
  context. Supported algorithms are MD5, SHA-256, and their session variants.
  Credentials are never negotiated with a different origin after a redirect.
- Fetch decompresses responses automatically, so `decode_content => false` cannot
  return the compressed wire bytes. The shim removes `Content-Encoding` and the
  encoded `Content-Length` from decoded responses. It also omits `Content-Length`
  on CORS responses, where the encoding header might be hidden. The decoded size
  remains available through transfer statistics; progress totals may be unknown.

Many unsupported curl options are accepted without changing transport behavior.
The reported libcurl version describes API compatibility, not support for every
native libcurl transport capability.
