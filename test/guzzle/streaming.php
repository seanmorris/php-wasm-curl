<?php

use GuzzleHttp\Client;
use GuzzleHttp\Handler\CurlHandler;
use GuzzleHttp\Handler\CurlMultiHandler;
use GuzzleHttp\HandlerStack;
use GuzzleHttp\Pool;
use GuzzleHttp\Promise\Utils;
use GuzzleHttp\Psr7\Request;

check('on_headers runs before the response finishes downloading', function () use ($client, $base) {
    $early = false;
    $probe = new Client(['base_uri' => $base, 'handler' => HandlerStack::create(new CurlHandler())]);
    $response = $client->get('/stream?id=early', ['on_headers' => function () use ($probe, &$early) {
        $early = !inspectBody($probe->get('/stream-state?id=early'))['complete'];
    }]);
    expect($early && $response->getBody()->getSize() === 8192, 'headers were delayed until the body finished');
});

check('on_headers rejection aborts the network download', function () use ($client) {
    try {
        $client->get('/stream?id=veto', ['on_headers' => function () { throw new RuntimeException('veto'); }]);
        throw new RuntimeException('header veto was ignored');
    } catch (GuzzleHttp\Exception\RequestException $expected) {
        expect($expected->getPrevious()?->getMessage() === 'veto', 'header veto was lost');
    }
    expect(!inspectBody($client->get('/stream-state?id=veto'))['complete'], 'veto downloaded the entire body');
});

check('download progress and file sinks advance while bytes arrive', function () use ($client) {
    $positions = [];
    $sink = fopen('/streamed-response.bin', 'w+');
    $client->get('/stream?id=progress&unknown=1', [
        'sink' => $sink,
        'progress' => function ($total, $received) use (&$positions, $sink) {
            if ($received > 0) $positions[(int) $received] = ftell($sink);
        },
    ]);
    expect(count($positions) > 1 && min(array_keys($positions)) < 8192, 'progress only reported the final size');
    foreach ($positions as $received => $written) expect($received === $written, 'sink did not receive the current chunk');
    expect(file_get_contents('/streamed-response.bin') === implode('', array_map(fn($i) => str_repeat(chr(65 + $i), 1024), range(0, 7))),
        'streamed response bytes changed');
    if (is_resource($sink)) fclose($sink);
});

check('progress callback abort stops an active curl transfer', function () use ($client, $base) {
    $curl = curl_init("$base/stream?id=abort");
    curl_setopt_array($curl, [CURLOPT_RETURNTRANSFER => true, CURLOPT_NOPROGRESS => false,
        CURLOPT_PROGRESSFUNCTION => function ($handle, $total, $received) { return $received >= 1024 ? 1 : 0; }]);
    expect(curl_exec($curl) === false && curl_errno($curl) === CURLE_ABORTED_BY_CALLBACK, 'progress abort errno differs');
    curl_close($curl);
    expect(!inspectBody($client->get('/stream-state?id=abort'))['complete'], 'progress abort waited for the entire body');
});

check('timeouts cover a response that has already delivered headers and bytes', function () use ($client) {
    $received = 0;
    try {
        $client->get('/stream?id=timeout', ['timeout' => 0.2,
            'progress' => function ($total, $downloaded) use (&$received) { $received = max($received, $downloaded); }]);
        throw new RuntimeException('body timeout did not reject');
    } catch (GuzzleHttp\Exception\ConnectException $error) {
        expect($error->getHandlerContext()['errno'] === 28, 'body timeout errno differs');
    }
    expect($received > 0 && $received < 8192, 'timeout did not preserve incremental progress');
});

check('Pool refills a fast slot before an unrelated slow request completes', function () use ($client, $base) {
    $completed = [];
    $requests = [new Request('GET', "$base/slow?ms=450&id=slow"),
        new Request('GET', "$base/slow?ms=20&id=fast"), new Request('GET', "$base/slow?ms=20&id=refill")];
    $pool = new Pool($client, $requests, ['concurrency' => 2,
        'fulfilled' => function ($response) use (&$completed) { $completed[] = (string) $response->getBody(); },
        'rejected' => function ($reason) { throw $reason; }]);
    $pool->promise()->wait();
    expect($completed === ['fast', 'refill', 'slow'], 'Pool completion order: '.json_encode($completed));
});

check('a pending promise can be cancelled during download', function () use ($client) {
    $promise = null;
    $promise = $client->getAsync('/stream?id=cancel', ['progress' => function ($total, $received) use (&$promise) {
        if ($received >= 1024) $promise->cancel();
    }]);
    $states = Utils::settle([$promise, $client->getAsync('/hello')])->wait();
    expect($states[0]['state'] === 'rejected' && $states[1]['state'] === 'fulfilled', 'active cancellation corrupted completions');
    expect(!inspectBody($client->get('/stream-state?id=cancel'))['complete'], 'cancelled request downloaded every chunk');
});

check('separate multi handles can run independently and clean up active requests', function () use ($base) {
    $handler = new CurlMultiHandler();
    $other = new Client(['base_uri' => $base, 'handler' => HandlerStack::create($handler)]);
    $promise = $other->getAsync('/stream?id=cleanup');
    $handler->tick();
    $promise->cancel();
    unset($promise, $other, $handler);
    $fresh = new Client(['base_uri' => $base]);
    expect((string) $fresh->get('/hello')->getBody() === 'Hello, curl!', 'cleanup left a stale transfer');
});

check('query encoding, DELETE, delayed requests, and non-throwing HTTP errors work', function () use ($client) {
    $start = microtime(true);
    $echo = inspectBody($client->delete('/inspect', ['delay' => 25, 'query' => ['a' => 'one two', 'b' => '&/']]));
    expect($echo['method'] === 'DELETE' && $echo['url'] === '/inspect?a=one%20two&b=%26%2F', 'query encoding or method differs');
    expect(microtime(true) - $start >= 0.02, 'delay was ignored');
    expect($client->get('/missing', ['http_errors' => false])->getStatusCode() === 404, 'http_errors false was ignored');
});

check('retry middleware reuses the transport after a server error', function () use ($base) {
    $stack = HandlerStack::create();
    $stack->push(GuzzleHttp\Middleware::retry(
        fn($retries, $request, $response) => $retries < 1 && $response && $response->getStatusCode() === 503,
        fn() => 10
    ));
    $retry = new Client(['base_uri' => $base, 'handler' => $stack]);
    expect((string) $retry->get('/flaky?id='.uniqid())->getBody() === '2', 'retry did not complete');
});

check('Digest authentication negotiates MD5 and SHA-256 including session and auth-int modes', function () use ($client) {
    foreach (['MD5', 'MD5-sess', 'SHA-256', 'SHA-256-sess'] as $algorithm) {
        foreach (['auth', 'auth-int'] as $qop) {
            $response = $client->post('/digest', ['query' => compact('algorithm', 'qop'), 'body' => "binary\x00\xff",
                'auth' => ['user', 'secret', 'digest']]);
            expect((string) $response->getBody() === 'digest authorized', "Digest $algorithm/$qop failed");
        }
    }
    expect((string) $client->get('/digest?stale=1', ['auth' => ['user', 'secret', 'digest']])->getBody() === 'digest authorized',
        'stale Digest challenge was not renewed');
    expect($client->get('/digest', ['auth' => ['user', 'wrong', 'digest'], 'http_errors' => false])->getStatusCode() === 401,
        'wrong Digest credentials did not return 401');
    foreach ([0, 55, 56, 63, 64, 65, 119, 120] as $length) {
        expect((string) $client->post('/digest?qop=auth-int', ['auth' => ['user', 'secret', 'digest'],
            'body' => str_repeat("\xff", $length)])->getBody() === 'digest authorized', 'Digest hash padding failed at '.$length);
    }
    foreach (['', '&userhash=1'] as $userhash) {
        expect((string) $client->get('/digest?algorithm=SHA-256&unicode=1'.$userhash,
            ['auth' => ["ü'ser", 'sëcret', 'digest']])->getBody() === 'digest authorized', 'UTF-8 Digest username or realm failed');
    }
    try {
        $client->get('/digest?algorithm=unsupported', ['auth' => ['user', 'secret', 'digest']]);
        throw new RuntimeException('unsupported Digest algorithm was accepted');
    } catch (GuzzleHttp\Exception\RequestException $error) {
        expect(($error->getHandlerContext()['errno'] ?? 0) === 4, 'unsupported Digest algorithm did not fail explicitly');
    }
});

check('auth negotiation permits Basic only when the caller allows it', function () use ($base) {
    foreach ([CURLAUTH_ANY => true, CURLAUTH_ANYSAFE => false, CURLAUTH_DIGEST => false] as $auth => $allowed) {
        $curl = curl_init("$base/basic-challenge");
        curl_setopt_array($curl, [CURLOPT_RETURNTRANSFER => true, CURLOPT_USERPWD => 'user:secret', CURLOPT_HTTPAUTH => $auth]);
        $body = curl_exec($curl);
        expect($body === ($allowed ? 'basic authorized' : 'challenge'), 'authentication was downgraded or incorrectly refused');
        expect(curl_getinfo($curl, CURLINFO_RESPONSE_CODE) === ($allowed ? 200 : 401), 'authentication status differs');
        curl_close($curl);
    }
});
