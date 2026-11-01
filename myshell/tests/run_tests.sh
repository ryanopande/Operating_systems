#!/bin/sh
# run_tests.sh — automated checks for the Phase 1 feedback fixes and the Phase 2
# client/server. run from the myshell directory after "make":  sh tests/run_tests.sh

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

# ---- Phase 2: client and server ----
"$ROOT/server" $PORT > server.log 2>&1 &
SERVER_PID=$!
sleep 1
printf 'echo hello\nunknowncmd\ncat < input.txt | sort | head -n 1\nexit\n' \
    | "$ROOT/client" 127.0.0.1 $PORT > client.log 2>&1
sleep 1

check "client shows command output"         "yes" "$(grep -q '^\$ hello$' client.log && echo yes)"
check "client shows the not-found message"  "yes" "$(grep -q 'Command not found: unknowncmd' client.log && echo yes)"
check "client shows a piped command result" "yes" "$(grep -q 'alpha' client.log && echo yes)"
check "server logged the connection"        "yes" "$(grep -q '^\[INFO\] Client connected\.' server.log && echo yes)"
check "server logged a received command"    "yes" "$(grep -q '^\[RECEIVED\] Received command: "echo hello" from client\.' server.log && echo yes)"
check "server logged an error"              "yes" "$(grep -q '^\[ERROR\] Command not found: "unknowncmd"' server.log && echo yes)"
if kill -0 $SERVER_PID 2>/dev/null; then
    check "server shut down after exit" "stopped" "running"; kill $SERVER_PID
else
    check "server shut down after exit" "stopped" "stopped"
fi

# the server must also stop when the client just disconnects (Ctrl-D / end of input)
"$ROOT/server" $PORT > server2.log 2>&1 &
SERVER_PID=$!
sleep 1
printf 'ls\n' | "$ROOT/client" 127.0.0.1 $PORT > /dev/null 2>&1
sleep 1
if kill -0 $SERVER_PID 2>/dev/null; then
    check "server shut down after client disconnect" "stopped" "running"; kill $SERVER_PID
else
    check "server shut down after client disconnect" "stopped" "stopped"
fi

cd "$ROOT" && rm -rf "$WORK"
echo; echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]