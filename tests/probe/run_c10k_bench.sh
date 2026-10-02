#!/bin/bash
# TR-8.1 编排：ulimit 调优 → RR c10k 探针 → 三点 RSS/fd 采样 → 10000 长连驱动
# 数据归档到 /tmp/t8_bench/（建连成功率/耗时、RSS/连接、worker 分布、错误数）
set -u
cd "$(dirname "$0")/../.."
N=${1:-10000}        # 连接数（默认 10000；冒烟可传 500）
REPO=$(pwd)
ARCH=/tmp/t8_bench
mkdir -p "$ARCH"
LOG=$ARCH/server.log
DRVLOG=$ARCH/driver.log
SAMPLE=$ARCH/samples.txt
: > "$LOG"; : > "$DRVLOG"; : > "$SAMPLE"

# ---- nofile 调优（两端继承同一软上限）----
ulimit -n 20000 || { echo "ulimit -n 20000 failed"; exit 1; }

echo "==== TR-8.1 c10k bench $(date) ====" | tee -a "$SAMPLE"
echo "host: $(sysctl -n hw.model 2>/dev/null) physcpu=$(sysctl -n hw.physicalcpu) \
logcpu=$(sysctl -n hw.ncpu) membytes=$(sysctl -n hw.memsize)" | tee -a "$SAMPLE"
echo "ulimit -n=$(ulimit -n) kern.maxfiles=$(sysctl -n kern.maxfiles) \
kern.maxfilesperproc=$(sysctl -n kern.maxfilesperproc)" | tee -a "$SAMPLE"

pkill -f "bin/lumyr tests/probe/c10k_server" 2>/dev/null
lsof -ti tcp:19610 2>/dev/null | xargs kill -9 2>/dev/null
sleep 0.5

./bin/lumyr tests/probe/c10k_server.lm >"$LOG" 2>&1 &
SRV=$!

for i in $(seq 1 80); do lsof -ti tcp:19610 >/dev/null 2>&1 && break; sleep 0.1; done
lsof -ti tcp:19610 >/dev/null 2>&1 || { echo "server not ready"; cat "$LOG"; exit 1; }

# RSS(KB) + TCP fd 数采样
sample() {
  rss=$(ps -o rss= -p $SRV 2>/dev/null | tr -d ' ')
  tcp=$(lsof -a -p $SRV -i TCP -F f 2>/dev/null | grep -c '^f')
  echo "$1: rss_kb=$rss rss_mb=$((rss/1024)) fd_tcp=$tcp t=$(date +%H:%M:%S)" | tee -a "$SAMPLE"
}

sleep 1.2
sample "SAMPLE-BASELINE"
rm -f /tmp/t8_bench/ping_done
C10K_PING_MARKER=/tmp/t8_bench/ping_done \
  python3 tests/probe/c10k_driver.py "$N" 65 19610 >"$DRVLOG" 2>&1 &
DRV=$!

# 等驱动 ping 轮完成标记（全部活+收发刚校验），立即采在役峰值（最多 150s）
for i in $(seq 1 1500); do [ -f /tmp/t8_bench/ping_done ] && break; sleep 0.1; done
if [ -f /tmp/t8_bench/ping_done ]; then
  sleep 0.5   # 等最近一条 2s 粒度 PROGRESS 落盘
  sample "SAMPLE-ACTIVE"
  echo "SAMPLE-ACTIVE-DIST $(grep 'STATE PROGRESS' "$LOG" | tail -1)" | tee -a "$SAMPLE"
else
  echo "SAMPLE-ACTIVE: ping marker timeout" | tee -a "$SAMPLE"
fi

wait $DRV; echo "driver rc=$?" | tee -a "$SAMPLE"

# 等服务端 STATE DRAINED（轮询日志，最多 60s），立即回落采样
for i in $(seq 1 600); do grep -q "STATE DRAINED" "$LOG" && break; sleep 0.1; done
sample "SAMPLE-DRAINED"

for i in $(seq 1 150); do kill -0 $SRV 2>/dev/null || break; sleep 0.1; done
echo "---- driver output ----" | tee -a "$SAMPLE"
grep -E "driver:" "$DRVLOG" | tee -a "$SAMPLE"
echo "---- server peak/dist markers ----" | tee -a "$SAMPLE"
grep -E "STATE BASELINE|STATE DRAINED|STATE EXIT|lumyr-app" "$LOG" | tee -a "$SAMPLE"
peakLine=$(grep 'STATE PROGRESS' "$LOG" | sed 's/.*peak=//' | sort -t= -k1 -nr | head -1)
echo "server peak line: $peakLine" | tee -a "$SAMPLE"
echo "archived: $SAMPLE"
