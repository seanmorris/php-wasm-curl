<?php

check('browser cookies follow the configured credential policy', function () use ($client, $credentialPolicy, $crossOrigin) {
    $response = $client->get('/browser-cookie');
    expect(!$response->hasHeader('Set-Cookie'), 'browser exposed Set-Cookie');
    $echo = inspectBody($client->get('/inspect', ['headers' => ['Cookie' => 'forged=php']]));
    $cookie = $echo['headers']['cookie'] ?? '';
    $expected = $credentialPolicy === 'include' || ($credentialPolicy === 'same-origin' && !$crossOrigin);
    expect(str_contains($cookie, 'browser=owned') === $expected, 'browser credential policy was ignored');
    expect(!str_contains($cookie, 'forged=php'), 'PHP overrode the browser cookie header');
});

check('browser refusal to follow redirects never contacts the next target', function () use ($client) {
    $before = inspectBody($client->get('/cors-state'))['final'];
    try {
        $client->get('/redirect', ['allow_redirects' => false]);
        throw new RuntimeException('browser followed a disabled redirect');
    } catch (GuzzleHttp\Exception\RequestException $error) {
        // PHP 8.4 does not expose the CURLE_NOT_BUILT_IN constant (libcurl code 4).
        expect(($error->getHandlerContext()['errno'] ?? 0) === 4,
            'opaque redirect did not report the browser limitation');
    }
    expect(inspectBody($client->get('/cors-state'))['final'] === $before, 'disabled redirect reached its destination');
});

check('explicit curl redirect following uses the browser transport', function () use ($base) {
	$following = new GuzzleHttp\Client(['base_uri' => $base, 'handler' => new GuzzleHttp\Handler\CurlHandler(),
		'curl' => [CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => -1]]);
	expect((string) $following->get('/redirect')->getBody() === 'final', 'Guzzle transport redirect opt-in failed');
    $curl = curl_init("$base/redirect");
    curl_setopt_array($curl, [CURLOPT_RETURNTRANSFER => true, CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => -1]);
    expect(curl_exec($curl) === 'final', 'explicit browser redirect did not follow');
    expect(str_ends_with(curl_getinfo($curl, CURLINFO_EFFECTIVE_URL), '/final'), 'browser effective URL missing');
    curl_close($curl);
});

check('a zero redirect limit fails before following the browser redirect', function () use ($client, $base) {
    $before = inspectBody($client->get('/cors-state'))['final'];
    $curl = curl_init("$base/redirect");
    curl_setopt_array($curl, [CURLOPT_RETURNTRANSFER => true, CURLOPT_FOLLOWLOCATION => true, CURLOPT_MAXREDIRS => 0]);
    expect(curl_exec($curl) === false && curl_errno($curl) === CURLE_TOO_MANY_REDIRECTS, 'zero redirect limit was ignored');
    curl_close($curl);
    expect(inspectBody($client->get('/cors-state'))['final'] === $before, 'zero redirect limit contacted the next target');
});

if ($crossOrigin) {
    check('CORS preflight permits JSON, authorization, and custom methods', function () use ($client) {
        $response = $client->patch('/inspect', ['json' => ['cors' => true], 'headers' => ['Authorization' => 'Bearer cors']]);
        $echo = inspectBody($response);
        expect($echo['method'] === 'PATCH' && $echo['headers']['authorization'] === 'Bearer cors', 'preflighted request failed');
        expect(inspectBody($client->get('/cors-state'))['preflights'] > 0, 'browser did not issue preflights');
    });
    check('CORS errors reject while explicitly exposed headers are readable', function () use ($client) {
        expect($client->get('/hello')->getHeaderLine('X-Test') === 'yes', 'exposed response header missing');
        try {
            $client->get('/cors-denied');
            throw new RuntimeException('CORS denial did not reject');
        } catch (GuzzleHttp\Exception\ConnectException $expected) {
        }
    });
}
