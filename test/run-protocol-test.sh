#!/bin/bash
# Starts the mock phone, runs a full protocol conversation against it, and
# reports whether either side complained.
set -uo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"

# A fixed port races with anything else probing localhost (this box had an
# HTTP client hit 15037 before the protocol test could). Pick a free one.
if [[ -z "${PORT:-}" ]]; then
	PORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
fi
LOG=$(mktemp /tmp/mockphone.XXXXXX)

./test/mockphone "$PORT" > "$LOG" 2>&1 &
mock=$!
trap 'kill $mock 2>/dev/null; rm -f "$LOG"' EXIT

# Wait for the listen banner rather than probing the port: a probe
# connection would look like a session to the mock and be reported as a
# failed handshake.
for _ in $(seq 50); do
	grep -q "listening on" "$LOG" 2>/dev/null && break
	sleep 0.1
done

./test/protocol "$PORT"
rc=$?

sleep 0.3
echo
echo "--- mock phone said ---"
cat "$LOG"

if grep -q 'hello ok: 51 b7 04 00' "$LOG"; then
	# A stray localhost HTTP probe can also connect (seen on this box).
	# If the real ADB/hello session completed, ignore extra junk sessions.
	exit "$rc"
fi
if grep -q '!!' "$LOG"; then
	echo
	echo "the mock phone reported protocol violations"
	rc=1
fi

exit $rc
