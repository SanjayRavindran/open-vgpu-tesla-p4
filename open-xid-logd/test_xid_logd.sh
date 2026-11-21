#!/usr/bin/env bash
set -euo pipefail

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BIN="$DIR/open-nvidia-xid-logd"
MOCK_SO="$DIR/mock_nvml.so"
TMP_ROOT="$(mktemp -d /tmp/test_xid_logd.XXXXXX)"
trap 'rm -rf "$TMP_ROOT"' EXIT

echo "=== Case 1: Pascal GP104 non-GSP GPU exits 0 & cleans PID file ==="
RT1="$TMP_ROOT/rt_non_gsp"
LOG1="$TMP_ROOT/non_gsp.log"
mkdir -p "$RT1"
RUNTIME_DIRECTORY="$RT1" \
  OPEN_XID_NVML_LIB="$MOCK_SO" \
  MOCK_NVML_MODE="non_gsp" \
  OPEN_XID_LOG_FILE="$LOG1" \
  OPEN_XID_FOREGROUND=1 \
  "$BIN"

grep -q "Non-GSP GPU detected at device 0. Exiting." "$LOG1"
grep -q "Shutdown (" "$LOG1"
test ! -e "$RT1/nvidia-xid-logd.pid"

echo "=== Case 2: GSP XID event triggers debugdump + 76-char Base64 logging + clean SIGTERM shutdown ==="
RT2="$TMP_ROOT/rt_gsp"
LOG2="$TMP_ROOT/gsp.log"
DUMP_TEMP="$TMP_ROOT/temp_dump.bin"
FAKE_DUMP="$TMP_ROOT/fake_debugdump.sh"
mkdir -p "$RT2"

cat >"$FAKE_DUMP" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
out=""
while [ $# -gt 0 ]; do
  if [ "$1" = "-f" ]; then
    shift
    out="$1"
  fi
  shift || true
done
python3 -c 'import sys, pathlib; pathlib.Path(sys.argv[1]).write_bytes(b"MOCK_NVLOG_" + bytes(range(65, 165)))' "$out"
EOF
chmod +x "$FAKE_DUMP"

RUNTIME_DIRECTORY="$RT2" \
  OPEN_XID_NVML_LIB="$MOCK_SO" \
  MOCK_NVML_MODE="gsp_xid" \
  OPEN_XID_LOG_FILE="$LOG2" \
  OPEN_XID_DEBUGDUMP="$FAKE_DUMP" \
  OPEN_XID_TEMP_DUMP="$DUMP_TEMP" \
  OPEN_XID_WAIT_MS=20 \
  OPEN_XID_FOREGROUND=1 \
  "$BIN" &
DAEMON_PID=$!

for _ in $(seq 1 40); do
  if grep -q "XID dump end" "$LOG2" 2>/dev/null; then
    break
  fi
  sleep 0.05
done
grep -q "XID 79 detected on device index: 0, initiating dump..." "$LOG2"
grep -q "XID dump start" "$LOG2"
grep -q "XID dump end" "$LOG2"
test ! -e "$DUMP_TEMP"

kill -TERM "$DAEMON_PID"
wait "$DAEMON_PID"
test ! -e "$RT2/nvidia-xid-logd.pid"
grep -q "Shutdown (" "$LOG2"

python3 - "$LOG2" <<'PY'
import base64, pathlib, sys
lines = pathlib.Path(sys.argv[1]).read_text().splitlines()
s = lines.index("XID dump start") + 1
e = lines.index("XID dump end")
chunks = lines[s:e]
assert len(chunks) >= 2
for c in chunks[:-1]:
    assert len(c) == 76, f"Expected 76-char chunk, got {len(c)}"
decoded = base64.b64decode("".join(chunks))
expected = b"MOCK_NVLOG_" + bytes(range(65, 165))
assert decoded == expected, f"Decoded mismatch: {decoded!r} vs {expected!r}"
print("Base64 76-char XID dump decode verified:", len(decoded), "bytes")
PY

echo "=== Case 3: Background daemonize (--fork default) non-GSP test ==="
RT3="$TMP_ROOT/rt_daemon"
LOG3="$TMP_ROOT/daemon.log"
mkdir -p "$RT3"
RUNTIME_DIRECTORY="$RT3" \
  OPEN_XID_NVML_LIB="$MOCK_SO" \
  MOCK_NVML_MODE="non_gsp" \
  OPEN_XID_LOG_FILE="$LOG3" \
  "$BIN"
for _ in $(seq 1 40); do
  if grep -q "Shutdown (" "$LOG3" 2>/dev/null; then
    break
  fi
  sleep 0.05
done
grep -q "Non-GSP GPU detected at device 0. Exiting." "$LOG3"
test ! -e "$RT3/nvidia-xid-logd.pid"

echo "test_xid_logd.sh: ALL PASSED"
