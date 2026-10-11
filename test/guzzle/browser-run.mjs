import fs from 'node:fs';
import path from 'node:path';
import {fileURLToPath} from 'node:url';
import {chromium, firefox, webkit} from 'playwright';
import {startServer} from '../server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const files = {};
function bundle(from, to)
{
	for(const entry of fs.readdirSync(from, {withFileTypes: true}))
	{
		const source = path.join(from, entry.name), target = to + '/' + entry.name;
		if(entry.isDirectory()) bundle(source, target);
		else files[target] = fs.readFileSync(source).toString('base64');
	}
}
bundle(path.join(here, 'vendor'), '/guzzle-vendor');
for(const name of ['test', 'streaming', 'browser']) files['/guzzle-' + name + '.php'] = fs.readFileSync(path.join(here, name + '.php')).toString('base64');
const fileJSON = JSON.stringify(files);
const assets = path.join(here, 'node_modules/php-wasm');
const extension = path.resolve(process.env.PHP_CURL_EXTENSION || path.join(here, '../build/php8.4-curl.so'));
fs.accessSync(extension);

const server = await startServer(null, {serve(request, response, url) {
	let file;
	if(url.pathname.startsWith('/assets/'))
	{
		const relative = decodeURIComponent(url.pathname.slice(8));
		file = path.resolve(assets, relative);
		if(!file.startsWith(assets + path.sep)) { response.writeHead(403); response.end(); return true; }
	}
	else if(url.pathname === '/curl.so') file = extension;
	else if(url.pathname === '/browser.mjs') file = path.join(here, 'browser.mjs');
	else if(url.pathname === '/files.json')
	{
		response.setHeader('Content-Type', 'application/json'); response.end(fileJSON); return true;
	}
	else if(url.pathname === '/')
	{
		response.setHeader('Content-Type', 'text/html');
		response.end('<!doctype html><title>php-wasm Guzzle tests</title><script type="module" src="/browser.mjs"></script>');
		return true;
	}
	else return false;
	response.setHeader('Content-Type', /\.(mjs|js)$/.test(file) ? 'text/javascript' : 'application/wasm');
	const stream = fs.createReadStream(file);
	stream.on('error', () => { response.writeHead(404); response.end(); });
	stream.pipe(response);
	return true;
}});
const origin = 'http://127.0.0.1:' + server.port;
const cross = await startServer(null, {corsOrigin: origin});
try
{
	for(const name of (process.env.BROWSERS || 'chromium,firefox,webkit').split(','))
	{
		const engine = {chromium, firefox, webkit}[name];
		if(!engine) throw new Error('Unknown browser: ' + name);
		const browser = await engine.launch({headless: true});
		try
		{
			for(const [crossOrigin, credentialPolicy] of [
				[false, 'same-origin'], [true, 'same-origin'],
				[true, 'include'], [false, 'omit'],
			])
			{
				const label = `${name} ${crossOrigin ? 'CORS' : 'same-origin'} credentials=${credentialPolicy}`;
				console.log('# ' + label);
				const context = await browser.newContext();
				let deadline;
				try
				{
					const page = await context.newPage();
					page.on('console', message => console.log(message.text().trimEnd()));
					page.on('pageerror', error => console.error(error));
					await page.goto(origin);
					await page.waitForFunction(() => typeof globalThis.runGuzzle === 'function');
					const config = {base: crossOrigin ? 'http://127.0.0.1:' + cross.port : origin, crossOrigin, credentialPolicy};
					const report = await Promise.race([
						page.evaluate(config => globalThis.runGuzzle(config), config),
						new Promise((_, reject) => { deadline = setTimeout(() => reject(new Error(label + ' timed out')), 120000); }),
					]);
					console.log('# ' + label + ': ' + JSON.stringify(report));
					if(report.failures || report.requests !== 0 || report.waiters !== 0) process.exitCode = 1;
				}
				finally { clearTimeout(deadline); await context.close(); }
			}
		}
		finally { await browser.close(); }
	}
}
finally { await Promise.all([server.close(), cross.close()]); }
