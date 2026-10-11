#!/usr/bin/env bash
set -euo pipefail

test_dir=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
tls_dir=$(mktemp -d)
trap 'rm -rf "$tls_dir"' EXIT

# Add the fixture's ephemeral certificate to the child Node process's trust store.
# HTTPS requests still use the runtime's normal certificate verification.
if ! openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj '/CN=localhost' \
	-addext 'subjectAltName=IP:127.0.0.1' \
	-keyout "$tls_dir/key.pem" -out "$tls_dir/cert.pem" >"$tls_dir/openssl.log" 2>&1; then
	cat "$tls_dir/openssl.log" >&2
	exit 1
fi

NODE_EXTRA_CA_CERTS="$tls_dir/cert.pem" PHP_CURL_TEST_TLS="$tls_dir" \
	"${NODE:-node}" "$test_dir/run.mjs"
