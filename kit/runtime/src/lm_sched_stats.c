// lm_sched_stats.c —— Phase 8.11：调度器可观测性实现
// 设计说明见 lm_sched_stats.h 头部注释。
// 本文件实现：pending_time 对数桶直方图 + LM_SCHED_DEBUG=trace:N 调试输出线程。
#include "lm_sched_stats.h"
#include "lm_scheduler.h"      /* registry 快照 / overflow_len / lm_wsq_size_approx */
#include "lm_blocking_pool.h"  /* blocking 池在役/空闲线程数 */
#include "lm_co.h"             /* LM_STACK_CLASS_SMALL/NORMAL */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

lm_sched_stats_global_t g_lm_sched_stats;

/* pending_time 对数桶边界（µs，单调递增；末索引为溢出桶下限）。
 * 覆盖 10µs（同线程快通道）~ 1s（严重失衡），12 桶。 */
static const uint64_t g_pending_bounds_us[LM_SCHED_PENDING_BUCKETS] = {
    10, 50, 100, 500, 1000, 5000, 10000, 50000, 100000, 500000, 1000000,
    UINT64_MAX   /* 溢出桶：>1s */
};

/* Task 6：栈高水位对数桶边界（字节，单调递增；末索引为溢出桶下限）。
 * 覆盖 1KiB（极浅 IO 转发）~ 128KiB（NORMAL 档限），9 桶；
 * 溢出桶 >128KiB 用于捕获超档异常（理论不应出现，出现即 guard 临近告警）。
 * 注：末档上界 UINT64_MAX 不参与打印（打印时替换为 0 表示"溢出"）。 */
static const uint64_t g_stack_hw_bounds[LM_SCHED_STACK_HW_BUCKETS] = {
    1024, 2048, 4096, 8192, 16384, 32768, 65536, 131072,
    UINT64_MAX   /* 溢出桶：>128KiB */
};

const uint64_t* lm_sched_stats_stack_hw_bounds(void) {
    return g_stack_hw_bounds;
}

void lm_sched_stats_record_stack_hw(int stack_class, uint64_t used_bytes) {
    int ci;
    if (stack_class == LM_STACK_CLASS_SMALL)       ci = 0;
    else if (stack_class == LM_STACK_CLASS_NORMAL) ci = 1;
    else return;
    /* 桶判定：顺序扫描（低桶命中即返——IO 协程水位集中在前几桶）。 */
    int idx = LM_SCHED_STACK_HW_BUCKETS - 1;
    for (int i = 0; i < LM_SCHED_STACK_HW_BUCKETS - 1; i++) {
        if (used_bytes <= g_stack_hw_bounds[i]) { idx = i; break; }
    }
    atomic_fetch_add_explicit(&g_lm_sched_stats.stack_hw_buckets[ci][idx], 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lm_sched_stats.stack_hw_count[ci], 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lm_sched_stats.stack_hw_sum[ci], used_bytes,
                              memory_order_relaxed);
    /* fetch_max：高水位只关心上界，CAS 循环宽松收敛（竞争极少，同档协程
     * 水位分布集中，max 很快触顶后恒为读）。 */
    uint64_t cur = atomic_load_explicit(&g_lm_sched_stats.stack_hw_max[ci],
                                        memory_order_relaxed);
    while (used_bytes > cur) {
        if (atomic_compare_exchange_weak_explicit(&g_lm_sched_stats.stack_hw_max[ci],
                &cur, used_bytes, memory_order_relaxed, memory_order_relaxed)) break;
    }
}

void lm_sched_stats_record_pending_ns(uint64_t ns) {
    uint64_t us = ns / 1000;
    /* 顺序扫描：低桶命中即返——正常负载 pending 集中在前几桶，平均比较 <3 次。 */
    int idx = LM_SCHED_PENDING_BUCKETS - 1;
    for (int i = 0; i < LM_SCHED_PENDING_BUCKETS - 1; i++) {
        if (us <= g_pending_bounds_us[i]) { idx = i; break; }
    }
    atomic_fetch_add_explicit(&g_lm_sched_stats.pending_buckets[idx], 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lm_sched_stats.pending_count, 1,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&g_lm_sched_stats.pending_sum_ns, ns,
                              memory_order_relaxed);
}

const uint64_t* lm_sched_stats_pending_bounds(void) {
    return g_pending_bounds_us;
}

/* ============================================================
 * trace 线程：LM_SCHED_DEBUG=trace:N 每 N ms 打印调度器全景。
 * ============================================================ */

static _Atomic int g_stats_started = 0;
static pthread_mutex_t g_stats_once_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 利用率采样：per-scheduler 忙/总样本计数（trace 线程独占写，跨轮累计差分）。
 * 采样信号：sleeping==0（compute worker）或 current!=NULL（IO scheduler 正在
 * 执行协程）。trace 轮隔即采样周期——N ms 粒度对排障足够，热路径零成本。 */
typedef struct trace_util_s {
    lm_scheduler_t* s;
    uint64_t busy;    /* 累计忙样本 */
    uint64_t total;   /* 累计总样本 */
} trace_util_t;

#define LM_TRACE_MAX_SCHEDS 128

/* 从窗口直方图计算分位数（返回桶上界 µs）。total==0 返回 0。 */
static uint64_t trace_percentile_us(const uint64_t* buckets, uint64_t total,
                                    double q) {
    if (total == 0) return 0;
    uint64_t target = (uint64_t)((double)total * q) + 1;
    uint64_t cum = 0;
    for (int i = 0; i < LM_SCHED_PENDING_BUCKETS; i++) {
        cum += buckets[i];
        if (cum >= target) return g_pending_bounds_us[i];
    }
    return g_pending_bounds_us[LM_SCHED_PENDING_BUCKETS - 1];
}



static void trace_print_once(trace_util_t* utils, int* util_n) {
    lm_scheduler_t* snap[LM_TRACE_MAX_SCHEDS];
    int n = lm_scheduler_registry_snapshot(snap, LM_TRACE_MAX_SCHEDS);

    /* 窗口直方图快照并清零（exchange 0：与记录方并发安全，丢失的增量计入下轮）。 */
    uint64_t buckets[LM_SCHED_PENDING_BUCKETS];
    for (int i = 0; i < LM_SCHED_PENDING_BUCKETS; i++) {
        buckets[i] = atomic_exchange_explicit(&g_lm_sched_stats.pending_buckets[i],
                                              0, memory_order_relaxed);
    }
    uint64_t pcount = atomic_exchange_explicit(&g_lm_sched_stats.pending_count, 0,
                                               memory_order_relaxed);
    uint64_t psum = atomic_exchange_explicit(&g_lm_sched_stats.pending_sum_ns, 0,
                                             memory_order_relaxed);

    int io_n = 0, worker_n = 0;
    for (int i = 0; i < n; i++) {
        if (snap[i]->reactor) io_n++; else worker_n++;
    }

    /* 全局行：scheduler 构成 / 存活协程 / 全局溢出深度 / blocking 池 /
     * reduction 账本 / pending 分位数（调度器健康度第一指标）。
     * Phase 8.13：stuck 计数（sysmon 丢唤醒确认累计，正常负载恒 0）。 */
    /* Task 6：栈高水位分档打印（累计值，不清零）。
     * 分位数/计数/总和均从同一快照计算，消除并发写导致的矛盾。 */
    uint64_t shwBuckets[LM_SCHED_STACK_HW_CLASSES][LM_SCHED_STACK_HW_BUCKETS];
    uint64_t shwCnt[2] = {0, 0};
    uint64_t shwSum[2] = {0, 0};
    for (int ci = 0; ci < LM_SCHED_STACK_HW_CLASSES; ci++) {
        for (int i = 0; i < LM_SCHED_STACK_HW_BUCKETS; i++) {
            uint64_t v = atomic_load_explicit(
                &g_lm_sched_stats.stack_hw_buckets[ci][i], memory_order_relaxed);
            shwBuckets[ci][i] = v;
            shwCnt[ci] += v;
        }
        shwSum[ci] = atomic_load_explicit(&g_lm_sched_stats.stack_hw_sum[ci],
                                          memory_order_relaxed);
    }
    uint64_t shwMax0 = atomic_load_explicit(&g_lm_sched_stats.stack_hw_max[0],
                                            memory_order_relaxed);
    uint64_t shwMax1 = atomic_load_explicit(&g_lm_sched_stats.stack_hw_max[1],
                                            memory_order_relaxed);
    /* 从快照算分位数（与 trace_stack_hw_percentile_b 同算法，但用快照值）。 */
    #define SHW_PCTL(ci, q) ({ \
        uint64_t _t = (uint64_t)((double)shwCnt[ci] * (q)) + 1; \
        uint64_t _c = 0; \
        uint64_t _r = 0; \
        for (int _i = 0; _i < LM_SCHED_STACK_HW_BUCKETS; _i++) { \
            _c += shwBuckets[ci][_i]; \
            if (_c >= _t) { _r = g_stack_hw_bounds[_i]; break; } \
        } \
        if (_r == 0 && shwCnt[ci] > 0) _r = g_stack_hw_bounds[LM_SCHED_STACK_HW_BUCKETS - 1]; \
        _r; \
    })

    fprintf(stderr,
        "[sched-trace] scheds=%d(io=%d,compute=%d) live_co=%ld overflow=%ld "
        "blocking=%d/%d force_yield=%llu long_sched=%llu stuck=%llu "
        "pending n=%llu avg=%lluus p50=%lluus p99=%lluus "
        "stackHw[S] n=%llu avg=%lluB p50=%lluB p99=%lluB max=%lluB "
        "stackHw[N] n=%llu avg=%lluB p50=%lluB p99=%lluB max=%lluB\n",
        n, io_n, worker_n,
        atomic_load_explicit(&g_lm_sched_stats.live_co, memory_order_relaxed),
        lm_scheduler_overflow_len(),
        lm_blocking_pool_size(), lm_blocking_pool_idle(),
        (unsigned long long)atomic_load_explicit(&g_lm_sched_stats.force_yield_count,
                                                 memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_lm_sched_stats.long_sched_count,
                                                 memory_order_relaxed),
        (unsigned long long)atomic_load_explicit(&g_lm_sched_stats.stuck_co_count,
                                                 memory_order_relaxed),
        (unsigned long long)pcount,
        pcount ? (unsigned long long)(psum / 1000 / pcount) : 0ULL,
        (unsigned long long)trace_percentile_us(buckets, pcount, 0.50),
        (unsigned long long)trace_percentile_us(buckets, pcount, 0.99),
        (unsigned long long)shwCnt[0],
        (unsigned long long)(shwCnt[0] ? shwSum[0] / shwCnt[0] : 0ULL),
        (unsigned long long)SHW_PCTL(0, 0.50),
        (unsigned long long)SHW_PCTL(0, 0.99),
        (unsigned long long)shwMax0,
        (unsigned long long)shwCnt[1],
        (unsigned long long)(shwCnt[1] ? shwSum[1] / shwCnt[1] : 0ULL),
        (unsigned long long)SHW_PCTL(1, 0.50),
        (unsigned long long)SHW_PCTL(1, 0.99),
        (unsigned long long)shwMax1);
    #undef SHW_PCTL

    /* 每 scheduler 行：队列深度分列（LIFO/本地 WSQ/mutex 定向）+ 窃取 +
     * 唤醒 + LIFO 触顶 + 利用率（采样占比）。 */
    for (int i = 0; i < n; i++) {
        lm_scheduler_t* s = snap[i];
        /* 利用率采样：忙 = 正在执行协程（current 非空）。compute worker 睡眠时
         * sleeping==1 且 current==NULL，IO scheduler 事件空转时 current==NULL——
         * 统一以 current 为准。 */
        int busy = atomic_load_explicit(&s->current, memory_order_relaxed) != NULL;
        uint64_t ub = 0, ut = 0;
        int found = -1;
        for (int k = 0; k < *util_n; k++) {
            if (utils[k].s == s) { found = k; break; }
        }
        if (found < 0 && *util_n < LM_TRACE_MAX_SCHEDS) {
            found = (*util_n)++;
            utils[found].s = s;
            utils[found].busy = 0;
            utils[found].total = 0;
        }
        unsigned util_pct = 0;
        if (found >= 0) {
            utils[found].busy += (uint64_t)busy;
            utils[found].total += 1;
            ub = utils[found].busy;
            ut = utils[found].total;
            util_pct = ut ? (unsigned)(ub * 100 / ut) : 0;
        }
        uint64_t att = atomic_load_explicit(&s->steal_attempts, memory_order_relaxed);
        uint64_t suc = atomic_load_explicit(&s->steal_success, memory_order_relaxed);
        fprintf(stderr,
            "  sched#%d %-4s q(lifo=%d,local=%ld,remote=%ld) steal=%llu/%llu(%u%%) "
            "wake=%llu lifo_cap=%llu util=%u%%\n",
            s->sched_id, s->reactor ? "io" : "cmp",
            s->lifo_slot ? 1 : 0,
            lm_wsq_size_approx(&s->wsq),
            (long)atomic_load_explicit(&s->ready_len, memory_order_relaxed),
            (unsigned long long)suc, (unsigned long long)att,
            att ? (unsigned)(suc * 100 / att) : 0,
            (unsigned long long)atomic_load_explicit(&s->wake_count,
                                                     memory_order_relaxed),
            (unsigned long long)atomic_load_explicit(&s->lifo_quota_hits,
                                                     memory_order_relaxed),
            util_pct);
    }
}

static void* trace_thread_main(void* arg) {
    long interval_ms = (long)(intptr_t)arg;
    trace_util_t utils[LM_TRACE_MAX_SCHEDS];
    int util_n = 0;
    struct timespec ts;
    ts.tv_sec = (time_t)(interval_ms / 1000);
    ts.tv_nsec = (long)((interval_ms % 1000) * 1000000L);
    for (;;) {
        nanosleep(&ts, NULL);
        trace_print_once(utils, &util_n);
    }
    return NULL;
}

void lm_sched_stats_start(void) {
    if (atomic_load_explicit(&g_stats_started, memory_order_acquire)) return;
    pthread_mutex_lock(&g_stats_once_mutex);
    if (g_stats_started) {
        pthread_mutex_unlock(&g_stats_once_mutex);
        return;
    }
    atomic_store_explicit(&g_stats_started, 1, memory_order_release);
    pthread_mutex_unlock(&g_stats_once_mutex);
    /* env 解析：LM_SCHED_DEBUG=trace:N（N=打印间隔 ms）。未设置/格式不符不启动。
     * 集中在本函数解析（本模块唯一 env 入口），热路径零 getenv。 */
    const char* e = getenv("LM_SCHED_DEBUG");
    if (!e || strncmp(e, "trace:", 6) != 0) return;
    long interval_ms = atol(e + 6);
    if (interval_ms <= 0) interval_ms = 1000;
    pthread_t tid;
    if (pthread_create(&tid, NULL, trace_thread_main, (void*)(intptr_t)interval_ms) == 0) {
        pthread_detach(tid);   /* 守护式：进程退出即终止，无需 stop 协议 */
    }
}
