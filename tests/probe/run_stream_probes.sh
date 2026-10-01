#!/bin/bash
# ============================================================
# 流式零分配收发探针一键联调
#
# 流程：
#   1. 生成确定性测试数据（全 'A' 的 1MB / 10MB）
#   2. 跑自包含单进程回归 T5 / T6 / T7（条件内赋值 + 分支栈修复）
#   3. 启动 stream_file_server（端口 19200），跑 T1 / T3 / T4
#      装了 node 则追加 T2（Node createReadStream 对端）
#   4. 服务端落盘文件全部 cmp 字节级校验
#
# 用法：tests/probe/run_stream_probes.sh
# 失败时保留 /tmp/lumin_probe_* 现场；全部通过后自动清理
# ============================================================
set -u

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"
PROBE="tests/probe"
BIN="./bin/lumyr"
PORT=19200
TMP_PREFIX="lumin_probe"

DATA_1M="/tmp/${TMP_PREFIX}_data_1m.bin"
DATA_10M="/tmp/${TMP_PREFIX}_data_10m.bin"
EXPECT_512K="/tmp/${TMP_PREFIX}_expect_512k.bin"
EXPECT_64K="/tmp/${TMP_PREFIX}_expect_64k.bin"
SERVER_LOG="/tmp/${TMP_PREFIX}_server.log"
UPLOAD_GLOB="/tmp/${TMP_PREFIX}_upload_*.bin"

fail() { echo "FAIL: $*" >&2; exit 1; }

[ -x "$BIN" ] || fail "找不到 $BIN，请先执行 ./build.sh"

echo "=== 0. 清理旧现场与占用端口 ==="
for p in 19200 19201 19202 19203; do
    pids=$(lsof -ti tcp:$p 2>/dev/null)
    [ -n "$pids" ] && kill -9 $pids 2>/dev/null
done
rm -f "$UPLOAD_GLOB" "$DATA_1M" "$DATA_10M" \
      "$EXPECT_512K" "$EXPECT_64K" "$SERVER_LOG" \
      "/tmp/${TMP_PREFIX}_stream_10m.bin" "/tmp/${TMP_PREFIX}_t7_received.bin" \
      "/tmp/${TMP_PREFIX}_cond_assign_128k.bin"

echo "=== 1. 生成测试数据 ==="
"$BIN" "$PROBE/stream_make_testdata.lm" || fail "造数失败"
dd if="$DATA_1M" of="$EXPECT_512K" bs=524288 count=1 2>/dev/null || fail "dd 512K 失败"
dd if="$DATA_1M" of="$EXPECT_64K" bs=65536 count=1 2>/dev/null || fail "dd 64K 失败"

echo "=== 2. 自包含单进程回归 T5 / T6 / T7 ==="
"$BIN" "$PROBE/cond_assign_recv_probe.lm" || fail "T5 失败"
"$BIN" "$PROBE/cond_assign_timeout_probe.lm" || fail "T6 失败"
"$BIN" "$PROBE/stream_large_cond_probe.lm" || fail "T7 失败"
cmp "/tmp/${TMP_PREFIX}_stream_10m.bin" "/tmp/${TMP_PREFIX}_t7_received.bin" || fail "T7 字节不一致"
echo "T7 cmp 字节一致"

echo "=== 3. 启动流式服务端（端口 ${PORT}）==="
"$BIN" "$PROBE/stream_file_server.lm" > "$SERVER_LOG" 2>&1 &
SRV_PID=$!

cleanup_server() {
    kill "$SRV_PID" 2>/dev/null
    # 给服务端 close 前的 sleep(200) 留时间后再强制收尸
    sleep 0.5
    kill -9 "$SRV_PID" 2>/dev/null
    wait "$SRV_PID" 2>/dev/null || true
}
trap cleanup_server EXIT

# 等监听就绪（最多 5 秒）
for i in $(seq 1 50); do
    lsof -ti tcp:$PORT >/dev/null 2>&1 && break
    sleep 0.1
done
lsof -ti tcp:$PORT >/dev/null 2>&1 || { cat "$SERVER_LOG"; fail "服务端未在 $PORT 就绪"; }

echo "=== 4. T1 基础流式传输（1MB / 10MB）==="
"$BIN" "$PROBE/stream_client_file_transfer.lm" || { cat "$SERVER_LOG"; fail "T1 失败"; }

echo "=== 5. T3 multipart 表单上传（512KB）==="
"$BIN" "$PROBE/stream_client_html_upload.lm" || { cat "$SERVER_LOG"; fail "T3 失败"; }

echo "=== 6. T4 50 并发上传（各 64KB）==="
"$BIN" "$PROBE/stream_client_concurrent.lm" || { cat "$SERVER_LOG"; fail "T4 失败"; }

NEXT_ID=54
if command -v node >/dev/null 2>&1; then
    echo "=== 7. T2 Node.js createReadStream 对端（1MB / 10MB）==="
    node "$PROBE/stream_client_node_upload.js" || { cat "$SERVER_LOG"; fail "T2 失败"; }
    NEXT_ID=56
else
    echo "=== 7. 跳过 T2（未找到 node）==="
fi

echo "=== 8. 落盘文件字节级校验 ==="
cmp "$DATA_1M"  "/tmp/${TMP_PREFIX}_upload_1.bin"  || fail "T1 1MB 落盘不一致"
cmp "$DATA_10M" "/tmp/${TMP_PREFIX}_upload_2.bin"  || fail "T1 10MB 落盘不一致"
cmp "$EXPECT_512K" "/tmp/${TMP_PREFIX}_upload_3.bin" || fail "T3 512KB 落盘不一致"

i=4
while [ $i -lt 54 ]; do
    cmp "$EXPECT_64K" "/tmp/${TMP_PREFIX}_upload_$i.bin" || fail "T4 upload_$i 落盘不一致"
    i=$((i + 1))
done
echo "T4 50 个并发落盘全部一致"

if [ "$NEXT_ID" -eq 56 ]; then
    cmp "$DATA_1M"  "/tmp/${TMP_PREFIX}_upload_54.bin" || fail "T2 1MB 落盘不一致"
    cmp "$DATA_10M" "/tmp/${TMP_PREFIX}_upload_55.bin" || fail "T2 10MB 落盘不一致"
    echo "T2 落盘一致"
fi

cleanup_server
trap - EXIT

echo "=== 9. 清理临时文件 ==="
rm -f $UPLOAD_GLOB "$DATA_1M" "$DATA_10M" \
      "$EXPECT_512K" "$EXPECT_64K" "$SERVER_LOG" \
      "/tmp/${TMP_PREFIX}_stream_10m.bin" "/tmp/${TMP_PREFIX}_t7_received.bin" \
      "/tmp/${TMP_PREFIX}_cond_assign_128k.bin"

echo "流式探针套件 ALL PASS"
