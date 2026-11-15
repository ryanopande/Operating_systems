#!/bin/sh
# run_tests.sh — automated checks for the Phase 1 feedback fixes and the Phase 3
# multithreaded client/server. run from the myshell directory after "make":  sh tests/run_tests.sh

ROOT=$(pwd)
WORK=$(mktemp -d)
PORT=${PORT:-9300}
PASS=0
FAIL=0

# check <description> <expected text> <actual text>
check() {
    if [ "$2" = "$3" ]; then
        PASS=$((PASS + 1)); echo "PASS  $1"
    else
        FAIL=$((FAIL + 1)); echo "FAIL  $1"; echo "  expected: $2"; echo "  actual:   $3"
    fi
}

cd "$WORK" || exit 1
printf 'beta\nalpha beta\nalpha\nbeta\n' > input.txt

# ---- Phase 1 feedback fixes ----
echo OLD > out.txt
"$ROOT/myshell" > /dev/null <<'SH'
echo A > first.txt > out.txt
exit
SH
check "echo A > first.txt > out.txt creates first.txt" "yes" "$([ -f first.txt ] && echo yes)"
check "first.txt is empty"                              "0"   "$(wc -c < first.txt | tr -d ' ')"
check "out.txt holds the output"                        "A"   "$(cat out.txt)"

out=$("$ROOT/myshell" 2>&1 <<'SH'
cat < missing.txt < input.txt
exit
SH
)
check "cat < missing.txt < input.txt reports missing.txt only" "$(printf '$ File not found.\n$ ')" "$out"

echo OLDERR > err.txt
out=$("$ROOT/myshell" 2>&1 <<'SH'
cat 2> err.txt < missing.txt
exit
SH
)
check "cat 2> err.txt < missing.txt prints nothing"  '$ $ ' "$out"
check "err.txt now holds the missing-file error"     "File not found." "$(cat err.txt)"

rm -f first.txt
out=$("$ROOT/myshell" 2>&1 <<'SH'
cat > first.txt < missing.txt
exit
SH
)
check "cat > first.txt < missing.txt reports the error" "$(printf '$ File not found.\n$ ')" "$out"
check "...but still creates first.txt first"            "yes" "$([ -f first.txt ] && echo yes)"

# ---- Phase 3: multithreaded client/server ----
# helper: true when the server process is still alive
alive() { kill -0 "$1" 2>/dev/null && echo yes || echo no; }

"$ROOT/server" $PORT > server.log 2>&1 &
SERVER_PID=$!
sleep 1

# one client sends exit: only its session ends
printf 'echo hello\nunknowncmd\ncat < input.txt | sort | head -n 1\nexit\n' \
    | "$ROOT/client" 127.0.0.1 $PORT > client.log 2>&1
sleep 1

check "client shows command output"         "yes" "$(grep -q '^\$ hello$' client.log && echo yes)"
check "client shows the not-found message"  "yes" "$(grep -q 'Command not found: unknowncmd' client.log && echo yes)"
check "client shows a piped command result" "yes" "$(grep -q 'alpha' client.log && echo yes)"
check "client prints Disconnected after exit" "yes" "$(grep -q 'Disconnected from server\.' client.log && echo yes)"
check "server logged client #1 connection"  "yes" "$(grep -q '^\[INFO\] Client #1 connected from 127\.0\.0\.1:[0-9]*\. Assigned to Thread-1\.' server.log && echo yes)"
check "server logged a received command"    "yes" "$(grep -q '^\[RECEIVED\] \[Client #1 - 127\.0\.0\.1:[0-9]*\] Received command: "echo hello"' server.log && echo yes)"
check "server logged executing"             "yes" "$(grep -q '^\[EXECUTING\] \[Client #1 - 127\.0\.0\.1:[0-9]*\] Executing command: "echo hello"' server.log && echo yes)"
check "server logged an error"              "yes" "$(grep -q '^\[ERROR\] \[Client #1 - 127\.0\.0\.1:[0-9]*\] Command not found: "unknowncmd"' server.log && echo yes)"
check "server logged the exit request"      "yes" "$(grep -q 'Client requested disconnect\. Closing connection\.' server.log && echo yes)"
check "server logged Client #1 disconnected" "yes" "$(grep -q '^\[INFO\] Client #1 disconnected\.' server.log && echo yes)"
check "server keeps running after exit"     "yes" "$(alive $SERVER_PID)"

# a client that just disconnects (end of input, like Ctrl-D) also leaves the server up
printf 'ls\n' | "$ROOT/client" 127.0.0.1 $PORT > /dev/null 2>&1
sleep 1
check "server logged plain disconnect"      "yes" "$(grep -q '^\[INFO\] Client #2 disconnected\.' server.log && echo yes)"
check "server keeps running after Ctrl-D"   "yes" "$(alive $SERVER_PID)"

# two clients at once: the slow one must not block the fast one
( printf 'sleep 3\necho slow\n'; sleep 4 ) | "$ROOT/client" 127.0.0.1 $PORT > slow.log 2>&1 &
SLOW_PID=$!
sleep 1
START=$(date +%s)
printf 'echo fast\nexit\n' | "$ROOT/client" 127.0.0.1 $PORT > fast.log 2>&1
ELAPSED=$(( $(date +%s) - START ))
check "fast client is not blocked by slow client" "yes" "$([ $ELAPSED -lt 3 ] && echo yes)"
check "fast client got its own output"            "yes" "$(grep -q '^\$ fast' fast.log && echo yes)"
wait $SLOW_PID
check "slow client still got its output"          "yes" "$(grep -q 'slow' slow.log && echo yes)"

# a client that sends exit while another client's command is still running must be
# disconnected at once, not when the other command ends
( sleep 1; printf 'exit\n' ) | "$ROOT/client" 127.0.0.1 $PORT > exit_b.log 2>&1 &
EXIT_B=$!
sleep 0.3
( printf 'sleep 3\n'; sleep 4 ) | "$ROOT/client" 127.0.0.1 $PORT > exit_a.log 2>&1 &
EXIT_A=$!
START=$(date +%s)
wait $EXIT_B
ELAPSED=$(( $(date +%s) - START ))
check "exit is not delayed by another client's running command" "yes" "$([ $ELAPSED -lt 3 ] && echo yes)"
wait $EXIT_A

# three clients in parallel: every client gets only its own output
PAR_PIDS=""
for n in A B C; do
    ( printf "echo token_$n\n"; sleep 1; printf 'exit\n' ) | "$ROOT/client" 127.0.0.1 $PORT > par_$n.log 2>&1 &
    PAR_PIDS="$PAR_PIDS $!"
done
wait $PAR_PIDS   # wait for the clients only, not for the server
for n in A B C; do
    check "client $n received token_$n"   "yes" "$(grep -q "token_$n" par_$n.log && echo yes)"
done
check "no output crossed between clients" "3" "$(cat par_A.log par_B.log par_C.log | grep -c 'token_')"
check "server still running after 3 clients" "yes" "$(alive $SERVER_PID)"
kill $SERVER_PID 2>/dev/null; wait $SERVER_PID 2>/dev/null

cd "$ROOT" && rm -rf "$WORK"
echo; echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
