#!/usr/bin/env python3
# TR-8.1 驱动：建 N 条长连 + 逐条首 echo 校验 + 保活期一轮全量轻量 ping，
# 保活到 HOLD 秒后全部关闭。统计建连成功率/耗时、收发错误数、关闭耗时。
#   用法：python3 c10k_driver.py [N=10000] [HOLD=65] [PORT=19610]
import asyncio, sys, time, os

N    = int(sys.argv[1]) if len(sys.argv) > 1 else 10000
HOLD = float(sys.argv[2]) if len(sys.argv) > 2 else 65.0
PORT = int(sys.argv[3]) if len(sys.argv) > 3 else 19610
BATCH = 100          # 每批并发建连数（平滑打满 backlog）
PING_AT = 25.0       # 自建连起点起，第 25s 做保活全量 ping（轻量收发）
# ping 轮完成标记：全部活、收发刚校验完，编排据此采 ACTIVE 峰值样本
PING_MARKER = os.environ.get("C10K_PING_MARKER", "/tmp/t8_bench/ping_done")

stats = {"ok": 0, "bad": 0, "err": 0, "pingOk": 0, "pingBad": 0, "live": []}

async def one(i):
    try:
        r, w = await asyncio.open_connection("127.0.0.1", PORT)
        msg = ("hello%d\n" % i).encode()
        w.write(msg)
        await w.drain()
        line = await r.readline()
        if line != msg:
            stats["bad"] += 1
        else:
            stats["ok"] += 1
        stats["live"].append((i, r, w))
    except Exception as e:
        stats["err"] += 1
        if stats["err"] <= 5:
            print("driver: conn error i=%d %r" % (i, e))

async def ping_round():
    # 全量轻量收发：分批 gather，每条连发独立标记，响应带编号可校验
    bad = 0
    async def pong(item):
        nonlocal bad
        i, r, w = item
        try:
            m = ("ping%d\n" % i).encode()
            w.write(m)
            await w.drain()
            line = await asyncio.wait_for(r.readline(), timeout=10)
            if line != m:
                bad += 1
        except Exception:
            bad += 1
    t0 = time.time()
    for b in range(0, len(stats["live"]), BATCH):
        await asyncio.gather(*(pong(x) for x in stats["live"][b:b + BATCH]))
    return bad, time.time() - t0

async def main():
    t0 = time.time()
    for b in range(0, N, BATCH):
        await asyncio.gather(*(one(i) for i in range(b, min(b + BATCH, N))))
        if (b // BATCH) % 10 == 0:
            print("driver: progress %d/%d live=%d t=%.1fs"
                  % (min(b + BATCH, N), N, stats["ok"], time.time() - t0))
    tconn = time.time() - t0
    succ = stats["ok"]
    print("driver: connect ok=%d bad=%d err=%d in %.2fs (%.0f conn/s)"
          % (succ, stats["bad"], stats["err"], tconn,
         (succ / tconn) if tconn > 0 else 0.0))

    # 保活期：等到 PING_AT 做一轮全量轻量 ping
    dt = (t0 + PING_AT) - time.time()
    if dt > 0:
        await asyncio.sleep(dt)
    bad, tping = await ping_round()
    stats["pingBad"] = bad
    stats["pingOk"] = succ - bad
    print("driver: keepalive ping ok=%d bad=%d round=%.2fs"
          % (stats["pingOk"], bad, tping))
    # 此刻全部活且收发零错误 → 打 ACTIVE 采样标记（关闭前）
    try:
        with open(PING_MARKER, "w") as f:
            f.write("%d\n" % succ)
        print("driver: ping marker written -> " + PING_MARKER)
    except Exception as me:
        print("driver: marker write failed: %r" % (me,))

    # 保活到 HOLD 墙钟点
    dt = (t0 + HOLD) - time.time()
    if dt > 0:
        await asyncio.sleep(dt)
    t1 = time.time()
    for i, r, w in stats["live"]:
        w.close()
    await asyncio.gather(*(w.wait_closed() for i, r, w in stats["live"]),
                         return_exceptions=True)
    print("driver: closed %d in %.2fs" % (len(stats["live"]), time.time() - t1))

    ok_all = (succ == N and stats["bad"] == 0 and stats["err"] == 0 and bad == 0)
    print("driver: RESULT %s (ok=%d/%d bad=%d err=%d pingBad=%d)"
          % ("PASS" if ok_all else "FAIL", succ, N,
             stats["bad"], stats["err"], bad))

asyncio.run(asyncio.wait_for(main(), timeout=180))
