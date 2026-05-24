#!/usr/bin/env bash
#
# Test suite for select-server
#
# Tests covered:
#   1. Single connection, single request
#   2. Multiple sequential requests on one connection
#   3. Multiple clients connected simultaneously
#   4. Partial-line buffering (data arrives without a newline first)
#   5. Client disconnects cleanly (server keeps running)
#   6. Rapid-fire many clients

set -euo pipefail

BASE_PORT=18081    # each test uses BASE_PORT + test_number for isolation
SERVER=./select-server
PASS=0
FAIL=0

# ── colours ──────────────────────────────────────────────────────────────────
RED='\033[0;31m'; GREEN='\033[0;32m'; RESET='\033[0m'

pass() { echo -e "${GREEN}PASS${RESET} $1"; PASS=$((PASS+1)); }
fail() { echo -e "${RED}FAIL${RESET} $1"; FAIL=$((FAIL+1)); }

# ── server lifecycle ─────────────────────────────────────────────────────────
SERVER_PID=""
CURRENT_PORT=""

start_server() {
    CURRENT_PORT=$((BASE_PORT + $1))   # unique port per test
    $SERVER "$CURRENT_PORT" &
    SERVER_PID=$!
    sleep 0.15     # let it bind and listen
}

stop_server() {
    kill "$SERVER_PID" 2>/dev/null || true
    wait "$SERVER_PID" 2>/dev/null || true
}

# Send printf-escaped lines to the server, collect response within 2 s.
send_request() {
    printf '%b' "$1" | timeout 2 nc 127.0.0.1 "$CURRENT_PORT" 2>/dev/null || true
}

# ─────────────────────────────────────────────────────────────────────────────
# Test 1 – single connection, single request
# ─────────────────────────────────────────────────────────────────────────────
echo "=== Test 1: single connection / single request ==="
start_server 1
response=$(send_request "hello\n")
stop_server

if echo "$response" | grep -qE '^Current time: [0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2}$'; then
    pass "response matches 'Current time: YYYY-MM-DD HH:MM:SS'"
else
    fail "unexpected response: '$response'"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 2 – multiple requests on one connection
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test 2: multiple requests on one connection ==="
start_server 2
response=$(send_request "req1\nreq2\nreq3\n")
stop_server

count=$(echo "$response" | grep -c "Current time:" || true)
if [ "$count" -eq 3 ]; then
    pass "received 3 time responses on a single long-lived connection"
else
    fail "expected 3 responses, got $count"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 3 – simultaneous clients
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test 3: 10 simultaneous clients ==="
start_server 3

tmpdir=$(mktemp -d)
pids=()
for i in $(seq 1 10); do
    (
        # Stagger slightly to create genuine overlap.
        sleep "0.0$(( RANDOM % 5 ))"
        send_request "client$i\n" > "$tmpdir/resp_$i.txt"
    ) &
    pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
stop_server

ok=0
for i in $(seq 1 10); do
    grep -qE 'Current time:' "$tmpdir/resp_$i.txt" 2>/dev/null && ok=$((ok+1)) || true
done
rm -rf "$tmpdir"

if [ "$ok" -eq 10 ]; then
    pass "all 10 simultaneous clients received a response"
else
    fail "only $ok/10 clients received a response"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 4 – partial-line buffering
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test 4: partial-line buffering (data sent byte-by-byte) ==="
start_server 4

# Open a persistent TCP connection via a coproc, trickle data character by
# character without a newline, then finally send '\n'.
coproc CONN { nc 127.0.0.1 "$CURRENT_PORT"; }
sleep 0.1

for char in t i m e '?'; do
    printf '%s' "$char" >&"${CONN[1]}"
    sleep 0.04
done
printf '\n' >&"${CONN[1]}"
sleep 0.2

# Drain available output with a short per-line timeout.
response=""
while IFS= read -r -t 0.3 line <&"${CONN[0]}"; do
    response="$response$line"
done || true

# Close the coproc write side so nc exits.
exec {CONN[1]}>&-
kill "$CONN_PID" 2>/dev/null || true
wait  "$CONN_PID" 2>/dev/null || true

stop_server

if echo "$response" | grep -qE 'Current time:'; then
    pass "server buffered partial data and replied only after newline arrived"
else
    fail "no response received after byte-by-byte send: '$response'"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 5 – abrupt client disconnect does not crash server
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test 5: abrupt client disconnect does not crash server ==="
start_server 5

# Connect and immediately disconnect (EOF with no data).
timeout 1 nc 127.0.0.1 "$CURRENT_PORT" </dev/null >/dev/null 2>&1 || true
sleep 0.1

# Server must still be alive and serving.
response=$(send_request "after_disconnect\n")
stop_server

if echo "$response" | grep -qE 'Current time:'; then
    pass "server survived abrupt disconnect and served the next client"
else
    fail "server did not respond after abrupt disconnect: '$response'"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test 6 – rapid-fire 50 clients
# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "=== Test 6: 50 rapid-fire clients ==="
start_server 6

tmpdir=$(mktemp -d)
pids=()
for i in $(seq 1 50); do
    ( send_request "ping\n" > "$tmpdir/r_$i.txt" ) &
    pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
stop_server

ok=0
for i in $(seq 1 50); do
    grep -qE 'Current time:' "$tmpdir/r_$i.txt" 2>/dev/null && ok=$((ok+1)) || true
done
rm -rf "$tmpdir"

if [ "$ok" -eq 50 ]; then
    pass "all 50 rapid-fire clients were served"
else
    fail "$ok/50 clients received a response"
fi

# ─────────────────────────────────────────────────────────────────────────────
echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "$FAIL" -eq 0 ]
