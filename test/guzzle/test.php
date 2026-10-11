<?php
declare(strict_types=1);

require '/guzzle-vendor/autoload.php';

use GuzzleHttp\Client;
use GuzzleHttp\Cookie\CookieJar;
use GuzzleHttp\Exception\ClientException;
use GuzzleHttp\Exception\ConnectException;
use GuzzleHttp\Exception\RequestException;
use GuzzleHttp\Exception\TooManyRedirectsException;
use GuzzleHttp\Handler\CurlHandler;
use GuzzleHttp\Handler\CurlMultiHandler;
use GuzzleHttp\HandlerStack;
use GuzzleHttp\Pool;
use GuzzleHttp\Promise\Utils;
use GuzzleHttp\Psr7\PumpStream;
use GuzzleHttp\Psr7\Request;

$number = $failures = 0;
$browser = $browser ?? false;
function expect(bool $condition, string $message): void
{
    if (!$condition) {
        throw new RuntimeException($message);
    }
}
function check(string $name, callable $test): void
{
    global $number, $failures;
    ++$number;
    try {
        $test();
        echo "ok $number - $name\n";
    } catch (Throwable $error) {
        ++$failures;
        echo "not ok $number - $name\n# ", get_class($error), ': ', $error->getMessage(), "\n";
    }
}
function inspectBody($response): array
{
    return json_decode((string) $response->getBody(), true, 512, JSON_THROW_ON_ERROR);
}

check('the real PHP curl extension uses this fetch-backed library', function () {
    expect(extension_loaded('curl'), 'ext/curl is missing');
    expect(str_contains(curl_version()['version'], 'php-wasm-fetch'), 'test loaded a different libcurl');
    echo '# PHP ', PHP_VERSION, '; Guzzle ', Composer\InstalledVersions::getPrettyVersion('guzzlehttp/guzzle'),
        '; ', curl_version()['version'], "\n";
});

// Test the handler Guzzle selects by default as well as both curl handlers explicitly.
$clients = [
    'default' => new Client(['base_uri' => $base]),
    'easy' => new Client(['base_uri' => $base, 'proxy' => '', 'handler' => HandlerStack::create(new CurlHandler())]),
    'multi' => new Client(['base_uri' => $base, 'proxy' => '', 'handler' => HandlerStack::create(new CurlMultiHandler())]),
];
foreach ($clients as $name => $client) {
    check("$name handler: GET, JSON, forms, HEAD, and handle reuse", function () use ($client) {
        expect((string) $client->get('/hello')->getBody() === 'Hello, curl!', 'GET body mismatch');
        $echo = inspectBody($client->post('/echo', ['json' => ['hello' => 'world']]));
        expect($echo['method'] === 'POST' && $echo['body'] === '{"hello":"world"}', 'JSON POST mismatch');
        expect($echo['headers']['content-type'] === 'application/json', 'JSON content type missing');
        $echo = inspectBody($client->post('/echo', ['form_params' => ['a' => 'one two', 'b' => 'three']]));
        expect($echo['body'] === 'a=one+two&b=three', 'form encoding mismatch');
        expect((string) $client->head('/hello')->getBody() === '', 'HEAD delivered a body');
        $echo = inspectBody($client->get('/inspect'));
        expect($echo['method'] === 'GET' && $echo['length'] === 0, 'reused handle retained upload options');
    });
}
$client = $clients['default'];

if (isset($secureBase)) check('HTTPS works with runtime certificate verification enabled', function () use ($client, $secureBase) {
    $response = $client->get("$secureBase/hello");
    expect($response->getStatusCode() === 200 && (string) $response->getBody() === 'Hello, curl!', 'HTTPS request failed');
});

check('multiple request header values and empty headers survive', function () use ($client) {
    $echo = inspectBody($client->get('/inspect', ['headers' => ['X-Values' => ['one', 'two'], 'X-Empty' => '']]));
    expect($echo['headers']['x-values'] === 'one, two', 'a repeated request header was lost');
    expect(array_key_exists('x-empty', $echo['headers']) && $echo['headers']['x-empty'] === '', 'empty header was lost');
});

check('small binary POST and PATCH bodies preserve NUL bytes', function () use ($client) {
    $body = "before\x00\xffafter";
    foreach (['POST', 'PATCH'] as $method) {
        $echo = inspectBody($client->request($method, '/inspect', ['body' => $body]));
        expect($echo['method'] === $method && $echo['sha256'] === hash('sha256', $body), 'binary body changed');
    }
});

check('large stream upload exercises CURLOPT_READFUNCTION', function () use ($client) {
    $body = str_repeat("abc\x00\xff", 220000);
    $stream = fopen('php://temp', 'w+');
    fwrite($stream, $body);
    rewind($stream);
    $echo = inspectBody($client->put('/inspect', ['body' => $stream]));
    expect($echo['length'] === strlen($body) && $echo['sha256'] === hash('sha256', $body), 'stream upload changed');
    if (is_resource($stream)) fclose($stream);
});

check('unknown-length non-seekable upload reaches EOF correctly', function () use ($client) {
    $remaining = "unknown\x00\xfflength";
    $expected = hash('sha256', $remaining);
    $stream = new PumpStream(function ($length) use (&$remaining) {
        if ($remaining === '') return false;
        $chunk = substr($remaining, 0, $length);
        $remaining = substr($remaining, strlen($chunk));
        return $chunk;
    });
    $echo = inspectBody($client->post('/inspect', ['body' => $stream]));
    expect($echo['sha256'] === $expected, 'unknown-length upload changed');
});

check('Guzzle multipart uploads preserve binary file contents', function () use ($client) {
    $body = "file\x00\xff\nbytes";
    $response = $client->post('/echo-bytes', ['multipart' => [
        ['name' => 'field', 'contents' => 'value'],
        ['name' => 'upload', 'contents' => $body, 'filename' => 'binary.dat'],
    ]]);
    $multipart = (string) $response->getBody();
    expect(str_contains($multipart, 'name="field"') && str_contains($multipart, 'filename="binary.dat"'), 'multipart metadata missing');
    expect(str_contains($multipart, "\r\n\r\n$body\r\n"), 'multipart bytes changed');
});

check('Basic authorization works through Guzzle', function () use ($client) {
    $echo = inspectBody($client->get('/inspect', ['auth' => ['user', 'secret']]));
    expect($echo['headers']['authorization'] === 'Basic '.base64_encode('user:secret'), 'Basic authorization missing');
});

if (!$browser) check('cookie jar receives separate Set-Cookie headers and redirect cookies', function () use ($client) {
    $jar = new CookieJar();
    $response = $client->get('/cookies', ['cookies' => $jar]);
    expect(count($response->getHeader('Set-Cookie')) === 2 && count($jar) === 2, 'cookies were combined or lost');
    $echo = inspectBody($client->get('/redirect-cookie', ['cookies' => $jar]));
    foreach (['first=one', 'second=two', 'redirect=kept'] as $cookie) {
        expect(str_contains($echo['headers']['cookie'] ?? '', $cookie), "cookie missing: $cookie");
    }
});

if (!$browser) check('redirect middleware follows, tracks, disables, and limits redirects', function () use ($client) {
    $response = $client->get('/redirect', ['allow_redirects' => ['track_redirects' => true]]);
    expect((string) $response->getBody() === 'final', 'redirect was not followed');
    expect(count($response->getHeader('X-Guzzle-Redirect-History')) === 1, 'redirect history missing');
    expect($client->get('/redirect', ['allow_redirects' => false])->getStatusCode() === 302, 'disabled redirect was followed');
    try {
        $client->get('/redirect-chain?remaining=2&id=guzzle', ['allow_redirects' => ['max' => 1]]);
        throw new RuntimeException('redirect limit was ignored');
    } catch (TooManyRedirectsException $expected) {
    }
});

if (!$browser) check('PSR-18 sendRequest preserves redirects and HTTP error responses', function () use ($client, $base) {
    expect($client->sendRequest(new Request('GET', "$base/redirect"))->getStatusCode() === 302, 'PSR-18 followed a redirect');
    expect($client->sendRequest(new Request('GET', "$base/missing"))->getStatusCode() === 404, 'PSR-18 did not return HTTP error');
});

check('HTTP errors retain their response body', function () use ($client) {
    try {
        $client->get('/missing');
        throw new RuntimeException('404 did not throw');
    } catch (ClientException $error) {
        expect($error->getResponse()->getStatusCode() === 404, 'wrong HTTP status');
        expect((string) $error->getResponse()->getBody() === 'not here', 'error response body missing');
    }
});

check('timeouts and connection errors reject with curl context', function () use ($client) {
    foreach ([['/hang', ['timeout' => 0.02], 28], ['http://127.0.0.1:1/', [], 7]] as [$url, $options, $errno]) {
        try {
            $client->get($url, $options);
            throw new RuntimeException('connection failure did not throw');
        } catch (ConnectException $error) {
            expect($error->getHandlerContext()['errno'] === $errno, 'curl errno missing from exception');
        }
    }
});

check('on_headers, on_stats, and progress callbacks run', function () use ($client) {
    $headers = $stats = $progress = false;
    $client->get('/hello', [
        'on_headers' => function ($response) use (&$headers) { $headers = $response->getStatusCode() === 200; },
        'on_stats' => function ($transfer) use (&$stats) {
            $stats = $transfer->hasResponse() && $transfer->getHandlerStats()['http_code'] === 200
                && $transfer->getTransferTime() >= 0 && $transfer->getEffectiveUri()->getPath() === '/hello';
        },
        'progress' => function ($total, $downloaded) use (&$progress) { $progress = $downloaded === 12; },
    ]);
    expect($headers && $stats && $progress, 'a transfer callback did not report the response');
});

check('on_headers exceptions reject the request', function () use ($client) {
    try {
        $client->get('/hello', ['on_headers' => function () { throw new RuntimeException('header veto'); }]);
        throw new RuntimeException('header callback did not abort');
    } catch (RequestException $error) {
        expect($error->getPrevious()?->getMessage() === 'header veto', 'header callback exception was lost');
    }
});

check('sink writes the response to the PHP filesystem', function () use ($client) {
    $client->get('/hello', ['sink' => '/guzzle-response.txt']);
    expect(file_get_contents('/guzzle-response.txt') === 'Hello, curl!', 'sink file contents differ');
});

check('compressed responses are decoded once with consistent headers', function () use ($client) {
    $response = $client->get('/gzip');
    expect((string) $response->getBody() === 'Hello, compressed!', 'gzip response not decoded');
    expect(!$response->hasHeader('Content-Encoding'), 'decoded response retained Content-Encoding');
    expect(!$response->hasHeader('Content-Length') || (int) $response->getHeaderLine('Content-Length') === 18,
        'decoded Content-Length incorrect');
});

check('debug output reaches the requested PHP stream', function () use ($client) {
    $debug = fopen('php://temp', 'w+');
    $client->get('/hello', ['debug' => $debug]);
    rewind($debug);
    expect(str_contains(stream_get_contents($debug), '/hello'), 'verbose request output is missing');
    fclose($debug);
});

check('async requests overlap and preserve individual responses', function () use ($client) {
    $client->get('/peak');
    $promises = [];
    for ($i = 0; $i < 3; ++$i) $promises[] = $client->getAsync("/slow?ms=50&id=$i");
    $responses = Utils::unwrap($promises);
    foreach ($responses as $i => $response) expect((string) $response->getBody() === (string) $i, 'async response assigned to wrong request');
    expect((int) (string) $client->get('/peak')->getBody() === 3, 'async requests did not overlap');
});

check('Pool refills concurrency slots and reuses handles across batches', function () use ($client, $base) {
    $client->get('/peak');
    $requests = (function () use ($base) {
        for ($i = 0; $i < 9; ++$i) yield new Request('GET', "$base/slow?ms=20&id=$i");
    })();
    $completed = [];
    $pool = new Pool($client, $requests, [
        'concurrency' => 3,
        'fulfilled' => function ($response, $index) use (&$completed) { $completed[$index] = (string) $response->getBody(); },
        'rejected' => function ($reason) { throw $reason; },
    ]);
    $pool->promise()->wait();
    ksort($completed);
    expect(array_values($completed) === array_map('strval', range(0, 8)), 'Pool lost a completion');
    expect((int) (string) $client->get('/peak')->getBody() === 3, 'Pool concurrency was not bounded at three');
});

check('cancelled and failed async requests do not corrupt pending completions', function () use ($client) {
    $cancelled = $client->getAsync('/hello');
    $cancelled->cancel();
    $results = Utils::settle([$cancelled, $client->getAsync('/hello'), $client->getAsync('/missing')])->wait();
    expect($results[0]['state'] === 'rejected' && $results[1]['state'] === 'fulfilled'
        && $results[2]['state'] === 'rejected', 'mixed async completion states differ');
    expect((string) $results[1]['value']->getBody() === 'Hello, curl!', 'remaining response was lost');
});

require '/guzzle-streaming.php';
if ($browser) require '/guzzle-browser.php';

echo "1..$number\n";
file_put_contents('/guzzle-results.json', json_encode(['tests' => $number, 'failures' => $failures], JSON_THROW_ON_ERROR));
exit($failures ? 1 : 0);
