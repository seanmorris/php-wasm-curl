import {PhpWeb} from '/assets/PhpWeb.mjs';

export async function run({base, credentialPolicy, crossOrigin})
{
	// Seed a browser-owned HttpOnly cookie independently of the PHP cookie jar.
	await fetch(base + '/browser-cookie', {credentials: 'include'});
	const php = new PhpWeb({version: '8.4', sharedLibs: [location.origin + '/curl.so'],
		libcurlFetchOptions: {credentials: credentialPolicy}});
	php.addEventListener('output', event => console.log(event.detail.join('')));
	php.addEventListener('error', event => console.error(event.detail.join('')));
	const module = await php.binary;
	const files = await (await fetch('/files.json')).json();
	for(const [name, data] of Object.entries(files))
	{
		module.FS.mkdirTree(name.slice(0, name.lastIndexOf('/')) || '/');
		module.FS.writeFile(name, Uint8Array.from(atob(data), ch => ch.charCodeAt(0)));
	}
	await php.run(`<?php $base = '${base}'; $browser = true;
		$credentialPolicy = '${credentialPolicy}'; $crossOrigin = ${crossOrigin ? 'true' : 'false'};
		require '/guzzle-test.php';`);
	const report = JSON.parse(module.FS.readFile('/guzzle-results.json', {encoding: 'utf8'}));
	return {...report, requests: module.libcurlFetch?.requests.size, waiters: module.libcurlFetch?.waiters.size};
}

globalThis.runGuzzle = run;
