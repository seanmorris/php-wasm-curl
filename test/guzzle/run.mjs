import fs from 'node:fs/promises';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {PhpNode} from 'php-wasm/PhpNode';
import {startServer} from '../server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const extension = path.resolve(process.env.PHP_CURL_EXTENSION || path.join(here, '../build/php8.4-curl.so'));
await fs.access(extension);
await fs.access(path.join(here, 'vendor/autoload.php'));

const server = await startServer();
let secureServer;
try
{
	const tlsDir = process.env.PHP_CURL_TEST_TLS;
	if(!tlsDir) throw new Error('Run this suite with npm test or make check-guzzle to configure its test CA.');
	secureServer = await startServer({
		cert: await fs.readFile(path.join(tlsDir, 'cert.pem')),
		key: await fs.readFile(path.join(tlsDir, 'key.pem')),
	});
	const php = new PhpNode({version: '8.4', sharedLibs: [extension]});
	php.addEventListener('output', event => process.stdout.write(event.detail.join('')));
	php.addEventListener('error', event => process.stderr.write(event.detail.join('')));
	await php.binary;

	const copyTree = async (from, to) => {
		await php.mkdir(to);
		for(const entry of await fs.readdir(from, {withFileTypes: true}))
		{
			const source = path.join(from, entry.name);
			const target = to + '/' + entry.name;
			if(entry.isDirectory()) await copyTree(source, target);
			else await php.writeFile(target, await fs.readFile(source));
		}
	};
	await copyTree(path.join(here, 'vendor'), '/guzzle-vendor');
	await php.writeFile('/guzzle-test.php', await fs.readFile(path.join(here, 'test.php')));
	await php.writeFile('/guzzle-streaming.php', await fs.readFile(path.join(here, 'streaming.php')));
	const code = await php.run(`<?php
		$base = 'http://127.0.0.1:${server.port}';
		$secureBase = 'https://127.0.0.1:${secureServer.port}';
		require '/guzzle-test.php';`);
	// The embedded SAPI can return zero even when PHP calls exit(1).
	const report = JSON.parse(await php.readFile('/guzzle-results.json', {encoding: 'utf8'}));
	process.exitCode = code || (report.failures ? 1 : 0);
}
finally
{
	await Promise.all([server.close(), secureServer?.close()]);
}
