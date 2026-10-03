#!/usr/bin/env bash
# 压测三期多进程编排：1 个 lumyr server 常驻 + 4 个独立 lumyr client 进程
# 同时施压（WS 小帧 / HTTP gzip 2MiB / POST 1MiB / 2MiB 文件下载）。
# 任一 client rc!=0 即失败；产物日志归档 /tmp/mp_stress/。
set -u
cd "$(dirname "$0")/../.."

PROCS=${1:-4}
ARCH=/tmp/mp_stress
mkdir -p "$ARCH"

echo "==== MP stress: ${PROCS} client processes ===="

# 清理残留
pkill -f "bin/lumyr tests/probe/mp_stress_server" 2>/dev/null
lsof -ti tcp:19430 2>/dev/null | xargs kill -9 2>/dev/null
sleep 0.5

ulimit -n 20000 2>/dev/null || true

# 起 server
./bin/lumyr tests/probe/mp_stress_server.lm >"$ARCH/server.log" 2>&1 &
SRV=$!

for i in $(seq 1 80); do
  grep -q "MP-SERVER-READY" "$ARCH/server.log" 2>/dev/null && break
  sleep 0.1
done
if ! grep -q "MP-SERVER-READY" "$ARCH/server.log"; then
  echo "server not ready / 服务端未就绪:"
  cat "$ARCH/server.log"
  kill "$SRV" 2>/dev/null
  exit 1
fi
echo "server ready (pid=$SRV)"

# 起 N 个 client 进程
pids=""
fail=0
for i in $(seq 1 "$PROCS"); do
  ./bin/lumyr tests/probe/mp_stress_client.lm >"$ARCH/client_$i.log" 2>&1 &
  pids="$pids $!"
done

for p in $pids; do
  if wait "$p"; then
    :
  else
    rc=$?
    echo "client pid=$p FAILED (rc=$rc)"
    fail=1
  fi
done

# 汇总
for i in $(seq 1 "$PROCS"); do
  echo "---- client_$i ----"
  grep -E "\[MP1\]|\[MP2|\[LF5\]|\[LF6\]|ALL PASS" "$ARCH/client_$i.log" || true
done

kill "$SRV" 2>/dev/null
wait "$SRV" 2>/dev/null

if [ "$fail" -ne 0 ]; then
  echo "==== MP stress FAILED，详见 $ARCH ===="
  exit 1
fi
echo "==== MP stress ALL PASS ($PROCS processes), logs: $ARCH ===="
