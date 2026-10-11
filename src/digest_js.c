/* HTTP Digest (RFC 7616). MD5 is retained solely for legacy Digest challenges;
   SHA-256 uses Web Crypto. No credentials are sent until a challenge arrives. */
#include <emscripten.h>
#include "internal.h"

EM_JS(void, lcf_js_request_auth, (int id, const char *username, const char *password, int basic_allowed), {
	const state = Module.libcurlFetch;
	state.requests.get(id).auth = {username: UTF8ToString(username), password: password ? UTF8ToString(password) : "", basic: !!basic_allowed};
	if(state.digest) return;

	const md5 = bytes => {
		const size = Math.ceil((bytes.length + 9) / 64) * 64;
		const padded = new Uint8Array(size);
		padded.set(bytes);
		padded[bytes.length] = 128;
		const words = new DataView(padded.buffer);
		words.setUint32(size - 8, (bytes.length * 8) >>> 0, true);
		words.setUint32(size - 4, Math.floor(bytes.length / 536870912), true);
		const state = [0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476];
		const shifts = [[7, 12, 17, 22], [5, 9, 14, 20], [4, 11, 16, 23], [6, 10, 15, 21]];
		for(let offset = 0; offset < size; offset += 64)
		{
			let [a, b, c, d] = state;
			for(let i = 0; i < 64; i++)
			{
				let f, g;
				if(i < 16) { f = (b & c) | (~b & d); g = i; }
				else if(i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
				else if(i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
				else { f = c ^ (b | ~d); g = (7 * i) % 16; }
				const sum = (a + f + Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296) + words.getUint32(offset + g * 4, true)) | 0;
				const shift = shifts[Math.floor(i / 16)][i % 4];
				[a, b, c, d] = [d, (b + ((sum << shift) | (sum >>> (32 - shift)))) | 0, b, c];
			}
			for(const [i, value] of [a, b, c, d].entries()) state[i] = (state[i] + value) | 0;
		}
		const output = new Uint8Array(16), view = new DataView(output.buffer);
		state.forEach((value, i) => view.setUint32(i * 4, value, true));
		return output;
	};
	const hex = bytes => Array.from(bytes, byte => byte.toString(16).padStart(2, '0')).join("");
	const quote = String.fromCharCode(34), slash = String.fromCharCode(92);
	const quoted = value => quote + value.split(slash).join(slash + slash).split(quote).join(slash + quote) + quote;
	const unsupported = message => { throw Object.assign(new Error(message), {curlCode: 4}); };

	state.digest = async (header, auth, method, url, body) => {
		/* Split only outside quoted strings: realms and qop lists contain commas. */
		const parts = [];
		let start = 0, inQuote = false, escaped = false;
		for(let i = 0; i < header.length; i++)
		{
			const ch = header[i];
			if(escaped) { escaped = false; continue; }
			if(inQuote && ch === slash) { escaped = true; continue; }
			if(ch === quote) inQuote = !inQuote;
			if(!inQuote && ch === ',') { parts.push(header.slice(start, i).trim()); start = i + 1; }
		}
		parts.push(header.slice(start).trim());
		const challenges = [];
		let current, basic = false;
		for(let part of parts)
		{
			if(/^Basic[ \t]+/i.test(part)) basic = true;
			if(/^Digest[ \t]+/i.test(part)) { current = {}; challenges.push(current); part = part.replace(/^Digest[ \t]+/i, ""); }
			const field = /^([a-z][a-z0-9_-]*)[ \t]*=[ \t]*(.*)$/i.exec(part);
			if(!field) { current = null; continue; }
			if(!current) continue;
			let value = field[2];
			if(value.startsWith(quote) && value.endsWith(quote))
			{
				let decoded = "";
				for(let i = 1; i < value.length - 1; i++)
				{
					if(value[i] === slash) i++;
					decoded += value[i];
				}
				value = decoded;
			}
			const key = field[1].toLowerCase();
			if(Object.prototype.hasOwnProperty.call(current, key)) unsupported('Duplicate Digest challenge parameter');
			current[key] = value;
		}
		if(!challenges.length)
		{
			if(!basic || !auth.basic) return null;
			const bytes = new TextEncoder().encode(auth.username + ':' + auth.password);
			return {header: 'Basic ' + btoa(Array.from(bytes, byte => String.fromCharCode(byte)).join("")), stale: false};
		}
		const algorithms = ['MD5', 'MD5-SESS', 'SHA-256', 'SHA-256-SESS'];
		challenges.sort((a, b) => algorithms.indexOf((b.algorithm || 'MD5').toUpperCase()) - algorithms.indexOf((a.algorithm || 'MD5').toUpperCase()));
		const challenge = challenges[0];
		const algorithm = (challenge.algorithm || 'MD5').toUpperCase();
		if(!algorithms.includes(algorithm)) unsupported('Unsupported HTTP Digest algorithm');
		if(challenge.realm === undefined || !challenge.nonce) unsupported('Incomplete HTTP Digest challenge');
		const utf8 = (challenge.charset || "").toUpperCase() === 'UTF-8';
		if(challenge.charset && !utf8) unsupported('Unsupported HTTP Digest charset');
		/* Headers expose bytes as a ByteString; charset applies to the hash input. */
		const realm = utf8 ? new TextDecoder('utf-8', {fatal: true}).decode(Uint8Array.from(challenge.realm, ch => ch.charCodeAt(0))) : challenge.realm;
		const encode = value => {
			if(utf8) return new TextEncoder().encode(value);
			if(Array.from(value).some(ch => ch.codePointAt(0) > 255)) unsupported('Digest challenge must advertise UTF-8 for these credentials');
			return Uint8Array.from(value, ch => ch.charCodeAt(0));
		};
		const hash = async value => {
			const bytes = typeof value === 'string' ? encode(value) : value;
			if(algorithm.startsWith('MD5')) return hex(md5(bytes));
			if(!globalThis.crypto || !globalThis.crypto.subtle) unsupported('SHA-256 Digest requires Web Crypto in a secure context');
			return hex(new Uint8Array(await crypto.subtle.digest('SHA-256', bytes)));
		};
		const qops = (challenge.qop || "").toLowerCase().split(',').map(value => value.trim());
		const qop = qops.includes('auth') ? 'auth' : qops.includes('auth-int') ? 'auth-int' : "";
		if(challenge.qop && !qop) unsupported('Unsupported HTTP Digest quality of protection');
		const cnonce = hex(crypto.getRandomValues(new Uint8Array(16)));
		const nc = '00000001';
		const username = utf8 ? auth.username.normalize('NFC') : auth.username;
		const password = utf8 ? auth.password.normalize('NFC') : auth.password;
		let a1 = await hash(username + ':' + realm + ':' + password);
		if(algorithm.endsWith('-SESS')) a1 = await hash(a1 + ':' + challenge.nonce + ':' + cnonce);
		const uri = url.pathname + url.search;
		let a2 = method + ':' + uri;
		if(qop === 'auth-int') a2 += ':' + await hash(body instanceof Blob ? new Uint8Array(await body.arrayBuffer()) : (body || new Uint8Array(0)));
		const response = await hash(a1 + ':' + challenge.nonce + ':' + (qop ? nc + ':' + cnonce + ':' + qop + ':' : "") + await hash(a2));
		const userhash = challenge.userhash === 'true';
		const user = userhash ? await hash(username + ':' + realm) : username;
		const extendedUser = encodeURIComponent(user).split("").map(ch => "!'()*".includes(ch) ? '%' + ch.charCodeAt(0).toString(16).toUpperCase() : ch).join("");
		const fields = [userhash || !Array.from(user).some(ch => ch.charCodeAt(0) > 127)
			? 'username=' + quoted(user) : "username*=UTF-8''" + extendedUser,
			'realm=' + quoted(challenge.realm), 'nonce=' + quoted(challenge.nonce),
			'uri=' + quoted(uri), 'response=' + quoted(response), 'algorithm=' + algorithm];
		if(challenge.opaque !== undefined) fields.push('opaque=' + quoted(challenge.opaque));
		if(qop) fields.push('qop=' + qop, 'nc=' + nc);
		if(qop || algorithm.endsWith('-SESS')) fields.push('cnonce=' + quoted(cnonce));
		if(userhash) fields.push('userhash=true');
		return {header: 'Digest ' + fields.join(', '), stale: challenge.stale === 'true'};
	};
});
