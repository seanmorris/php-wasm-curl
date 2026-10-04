import http from 'node:http';

/**
 * Starts the HTTP fixture the API tests talk to.
 * @returns {Promise<{port: number, close: () => Promise<void>}>}
 */
export const startServer = () => new Promise(resolve => {
	let active = 0;
	let peak = 0;

	const server = http.createServer((request, response) => {
		const url = new URL(request.url, 'http://localhost');
		const chunks = [];
		request.on('data', chunk => chunks.push(chunk));
		request.on('end', () => {
			const body = Buffer.concat(chunks);

			switch(url.pathname)
			{
				case '/hello':
					response.writeHead(200, {'Content-Type': 'text/plain', 'X-Test': 'yes'});
					response.end('Hello, curl!');
					break;

				case '/echo':
					response.writeHead(200, {'Content-Type': 'application/json'});
					response.end(JSON.stringify({
						method: request.method,
						headers: request.headers,
						body: body.toString('latin1'),
					}));
					break;

				case '/redirect':
					response.writeHead(302, {Location: '/final'});
					response.end();
					break;

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

	server.listen(0, '127.0.0.1', () => resolve({
		port: server.address().port,
		close: () => new Promise(done => server.close(done)),
	}));
});
