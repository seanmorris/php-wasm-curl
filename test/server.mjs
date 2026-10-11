import http from 'node:http';
import https from 'node:https';
import {createHash} from 'node:crypto';
import {gzipSync} from 'node:zlib';

/**
 * Starts the HTTP (or HTTPS, with TLS options) fixture the tests talk to.
 * @returns {Promise<{port: number, close: () => Promise<void>}>}
 */
export const startServer = (tls = null, options = {}) => new Promise((resolve, reject) => {
	let active = 0;
	let peak = 0;
	const basicAuth = new Map();
	const redirectVisits = new Map();
	const streams = new Map();
	const visits = new Map();
	const attempts = new Map();
	let preflights = 0;

	const server = (tls ? https : http).createServer(tls || {}, (request, response) => {
		const url = new URL(request.url, 'http://localhost');
		if(options.serve && options.serve(request, response, url)) return;
		visits.set(url.pathname, (visits.get(url.pathname) || 0) + 1);
		if(options.corsOrigin && url.pathname !== '/cors-denied')
		{
			response.setHeader('Access-Control-Allow-Origin', options.corsOrigin);
			response.setHeader('Access-Control-Allow-Credentials', 'true');
			response.setHeader('Access-Control-Allow-Methods', 'GET, HEAD, POST, PUT, PATCH, DELETE, OPTIONS');
			response.setHeader('Access-Control-Allow-Headers', request.headers['access-control-request-headers'] || '*');
			response.setHeader('Access-Control-Expose-Headers', 'X-Test');
		}
		if(request.method === 'OPTIONS')
		{
			preflights++;
			response.writeHead(204);
			response.end();
			return;
		}
		const chunks = [];
		request.on('data', chunk => chunks.push(chunk));
		request.on('end', () => {
			const body = Buffer.concat(chunks);

			switch(url.pathname)
			{
				case '/basic-challenge':
					if(request.headers.authorization === 'Basic ' + Buffer.from('user:secret').toString('base64')) response.end('basic authorized');
					else
					{
						response.setHeader('Access-Control-Expose-Headers', 'WWW-Authenticate');
						response.writeHead(401, {'WWW-Authenticate': 'Basic realm="curl"'});
						response.end('challenge');
					}
					break;

				case '/flaky':
				{
					const id = url.searchParams.get('id');
					const tried = (attempts.get(id) || 0) + 1;
					attempts.set(id, tried);
					response.writeHead(tried === 1 ? 503 : 200);
					response.end(String(tried));
					break;
				}

				case '/digest':
				{
					const algorithm = url.searchParams.get('algorithm') || 'MD5';
					const qop = url.searchParams.get('qop') || 'auth';
					const unicode = url.searchParams.has('unicode');
					const realm = unicode ? 'réalm' : 'curl, test';
					const username = unicode ? "ü'ser" : 'user';
					const password = unicode ? 'sëcret' : 'secret';
					const userhash = url.searchParams.has('userhash');
					const nonce = url.searchParams.has('stale') && request.headers.authorization ? 'renewed' : 'nonce';
					const fields = Object.fromEntries(Array.from((request.headers.authorization || '').matchAll(/(\w+\*?)=(?:"([^"]*)"|([^,\s]+))/g), match => [match[1], match[2] ?? match[3]]));
					const hash = value => createHash(algorithm.toLowerCase().replace('-sess', '').replace('-', '')).update(value).digest('hex');
					let valid = false;
					if(fields.response && /^(MD5|SHA-256)(-sess)?$/i.test(algorithm))
					{
						let a1 = hash(username + ':' + realm + ':' + password);
						if(algorithm.endsWith('-sess')) a1 = hash(a1 + ':' + nonce + ':' + fields.cnonce);
						const a2 = hash(request.method + ':' + request.url + (qop === 'auth-int' ? ':' + hash(body) : ''));
						const digest = hash(a1 + ':' + nonce + ':' + fields.nc + ':' + fields.cnonce + ':' + qop + ':' + a2);
						const actualUser = fields['username*'] ? decodeURIComponent(fields['username*'].slice(7)) : fields.username;
						valid = actualUser === (userhash ? hash(username + ':' + realm) : username)
							&& fields.uri === request.url && fields.nonce === nonce && fields.qop === qop && fields.response === digest;
					}
					if(valid) response.end('digest authorized');
					else
					{
						response.setHeader('Access-Control-Expose-Headers', 'WWW-Authenticate, X-Test');
						const realmBytes = unicode ? Buffer.from(realm).toString('latin1') : realm;
						response.writeHead(401, {'WWW-Authenticate': `Digest realm="${realmBytes}", nonce="${nonce}", algorithm=${algorithm}, qop="${qop}", opaque="opaque", stale=${nonce === 'renewed'}${unicode ? ', charset=UTF-8' : ''}${userhash ? ', userhash=true' : ''}`});
						response.end('challenge');
					}
					break;
				}

				case '/stream':
				{
					const count = Number(url.searchParams.get('chunks') || 8);
					const size = Number(url.searchParams.get('size') || 1024);
					const state = {sent: 0, closed: false, complete: false};
					streams.set(url.searchParams.get('id'), state);
					response.setHeader('Content-Type', 'application/octet-stream');
					if(!url.searchParams.has('unknown')) response.setHeader('Content-Length', count * size);
					response.flushHeaders();
					const timer = setInterval(() => {
						response.write(Buffer.alloc(size, 65 + state.sent % 26));
						if(++state.sent === count)
						{
							state.complete = true;
							clearInterval(timer);
							response.end();
						}
					}, Number(url.searchParams.get('ms') || 60));
					response.on('close', () => { state.closed = true; clearInterval(timer); });
					break;
				}

				case '/stream-state':
					response.setHeader('Content-Type', 'application/json');
					response.end(JSON.stringify(streams.get(url.searchParams.get('id')) || null));
					break;

				case '/cors-state':
					response.setHeader('Content-Type', 'application/json');
					response.end(JSON.stringify({preflights, final: visits.get('/final') || 0}));
					break;

				case '/cors-denied':
					response.end('not readable across origins');
					break;

				case '/browser-cookie':
					response.setHeader('Set-Cookie', 'browser=owned; Path=/; SameSite=Lax; HttpOnly');
					response.end('cookie');
					break;

				case '/hello':
					response.writeHead(200, {'Content-Type': 'text/plain', 'X-Test': 'yes'});
					response.end('Hello, curl!');
					break;

				case '/long-status':
					response.writeHead(200, 'A'.repeat(600), {'Content-Type': 'text/plain'});
					response.end('long status');
					break;

				case '/echo':
					response.writeHead(200, {'Content-Type': 'application/json'});
					response.end(JSON.stringify({
						method: request.method,
						headers: request.headers,
						body: body.toString('latin1'),
					}));
					break;

				case '/echo-bytes':
					response.writeHead(200, {'Content-Type': 'application/octet-stream'});
					response.end(body);
					break;

				case '/inspect':
					response.setHeader('Content-Type', 'application/json');
					response.end(JSON.stringify({
						method: request.method, url: request.url, headers: request.headers,
						length: body.length, sha256: createHash('sha256').update(body).digest('hex'),
					}));
					break;

				case '/cookies':
					response.setHeader('Set-Cookie', ['first=one; Path=/', 'second=two; Path=/; HttpOnly']);
					response.end('cookies');
					break;

				case '/redirect-cookie':
					response.writeHead(302, {Location: '/inspect', 'Set-Cookie': 'redirect=kept; Path=/'});
					response.end();
					break;

				case '/gzip':
				{
					const compressed = gzipSync('Hello, compressed!');
					response.writeHead(200, {'Content-Type': 'text/plain', 'Content-Encoding': 'gzip', 'Content-Length': compressed.length});
					response.end(compressed);
					break;
				}

				case '/auth-probe':
				{
					const id = url.searchParams.get('id');
					if(/^Basic /i.test(request.headers.authorization || ''))
					{
						basicAuth.set(id, (basicAuth.get(id) || 0) + 1);
					}
					response.end('auth probe');
					break;
				}

				case '/auth-observed':
				{
					const id = url.searchParams.get('id');
					response.end(String(basicAuth.get(id) || 0));
					basicAuth.delete(id);
					break;
				}

				case '/redirect':
					response.writeHead(302, {Location: '/final'});
					response.end();
					break;

				case '/redirect-chain':
				{
					const id = url.searchParams.get('id');
					const remaining = Number(url.searchParams.get('remaining'));
					redirectVisits.set(id, (redirectVisits.get(id) || 0) + 1);
					if(remaining > 0)
					{
						url.searchParams.set('remaining', String(remaining - 1));
						response.writeHead(302, {Location: url.pathname + url.search});
						response.end();
					}
					else response.end('final');
					break;
				}

				case '/redirect-visits':
				{
					const id = url.searchParams.get('id');
					response.end(String(redirectVisits.get(id) || 0));
					redirectVisits.delete(id);
					break;
				}

				case '/final':
					response.writeHead(200, {'Content-Type': 'text/plain'});
					response.end('final');
					break;

				case '/missing':
					response.writeHead(404, {'Content-Type': 'text/plain'});
					response.end('not here');
					break;

				case '/hang':
					setTimeout(() => response.end(), 2000);
					break;

				case '/slow':
				{
					active++;
					peak = Math.max(peak, active);
					setTimeout(() => {
						active--;
						response.writeHead(200, {'Content-Type': 'text/plain'});
						response.end(url.searchParams.get('id') || 'slow');
					}, Number(url.searchParams.get('ms') || 300));
					break;
				}

				case '/peak':
					response.writeHead(200, {'Content-Type': 'text/plain'});
					response.end(String(peak));
					peak = 0;
					break;

				default:
					response.writeHead(500);
					response.end('unknown fixture');
			}
		});
	});

	server.on('error', reject);
	server.listen(0, '127.0.0.1', () => resolve({
		port: server.address().port,
		close: () => new Promise(done => server.close(done)),
	}));
});
