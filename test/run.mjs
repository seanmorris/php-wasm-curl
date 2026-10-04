import path from 'node:path';
import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { startServer } from './server.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const require = createRequire(import.meta.url);
const server = await startServer();

let exitCode = 1;
try
{
	const factory = require(path.join(here, 'build', 'main.js'));
	const module = await factory({noExitRuntime: true});
	exitCode = await module.ccall('start', 'number', ['number'], [server.port], {async: true}) ? 1 : 0;
}
catch(error)
{
	console.error(error);
}
finally
{
	await server.close();
}

process.exit(exitCode);
