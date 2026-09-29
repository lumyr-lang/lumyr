// lm_sched_stats.h —— Phase 8.11：调度器可观测性（指标族 + 排队延迟 + 调试输出）
// 对标 bthread bvar 指标族（task_control.cpp:204-304）+ pending_time 延迟记账
// （task_control.cpp:759-764）+ Go GODEBUG=schedtrace（proc.go:6950 区域）。
//
// 设计原则：热路径 per-thread / 全局无锁原子计数（bvar 模式），后台 trace 线程
// 聚合打印——可观测性不以热路径加锁为代价。
//
// 指标清单（对齐调研文档 8.11）：
//   每 scheduler（计数器内嵌 lm_scheduler_t 末尾，ABI 原则）：窃取尝试/成功次数、
//     唤醒次数、LIFO 配额触顶次数、mutex 定向队列深度（ready_len）。
//     队列深度其余分量（LIFO slot / 本地 WSQ）与利用率由 trace 线程采样得出。
//   全局（本文件 g_lm_sched_stats）：存活协程数、强制让出总次数、长调度告警次数、
//     pending_time 直方图（ready→被执行延迟分布，调度器健康度第一指标）。
//   worker 数 / 全局溢出深度 / blocking 池线程数：trace 打印时经既有 API
//     （registry 快照 / lm_scheduler_overflow_len / lm_blocking_pool_size/idle）采集，
//     不重复记账。
//
// 调试输出：LM_SCHED_DEBUG=trace:N 每 N 毫秒打印一行调度器全景
// （对齐 GODEBUG=schedtrace）——线上排障第一工具。未设置则不启动 trace 线程，
// 计数器照常累加（查询接口仍可用）。
#ifndef LM_SCHED_STATS_H
#define LM_SCHED_STATS_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* pending_time 直方图桶数（对数桶，边界见 lm_sched_stats.c g_pending_bounds_us）。
 * 桶语义：buckets[i] = 延迟 ∈ (bounds[i-1], bounds[i]] µs，末桶为溢出桶。 */
#define LM_SCHED_PENDING_BUCKETS 12

typedef struct lm_sched_stats_global_s {
    _Atomic long live_co;               /* 存活协程数（对齐 bthread_count） */
    _Atomic uint64_t force_yield_count; /* reduction 预算耗尽强制让出重入队总次数 */
    _Atomic uint64_t long_sched_count;  /* 长调度墙钟告警总次数（8.5 C 落地） */
    /* Phase 8.13：sysmon 确认的 stuck 协程（丢唤醒）累计次数。
     * 等待源已完成但协程仍悬挂、连续两轮扫描确认才 +1（两轮确认吸收
     * 投递在飞瞬态，正常负载恒为 0；>0 即存在唤醒协议 bug 或救援事件）。 */
    _Atomic uint64_t stuck_co_count;
    /* pending_time 直方图（对齐 LatencyRecorder）：ready→被执行延迟分布。
     * trace 线程每轮打印后清零（窗口语义）；trace 未启用时累计不清零。 */
    _Atomic uint64_t pending_buckets[LM_SCHED_PENDING_BUCKETS];
    _Atomic uint64_t pending_count;     /* 窗口内样本数 */
    _Atomic uint64_t pending_sum_ns;    /* 窗口内样本总延迟（均值用） */
} lm_sched_stats_global_t;

extern lm_sched_stats_global_t g_lm_sched_stats;

/* ===== 热路径记账（static inline 零调用帧，relaxed 原子） ===== */

static inline void lm_sched_stats_co_created(void) {
    atomic_fetch_add_explicit(&g_lm_sched_stats.live_co, 1, memory_order_relaxed);
}
static inline void lm_sched_stats_co_destroyed(void) {
    atomic_fetch_sub_explicit(&g_lm_sched_stats.live_co, 1, memory_order_relaxed);
}
static inline void lm_sched_stats_force_yield(void) {
    atomic_fetch_add_explicit(&g_lm_sched_stats.force_yield_count, 1,
                              memory_order_relaxed);
}
static inline void lm_sched_stats_long_sched(void) {
    atomic_fetch_add_explicit(&g_lm_sched_stats.long_sched_count, 1,
                              memory_order_relaxed);
}

/* Phase 8.13：stuck 协程计数——sysmon 确认丢唤醒时 +1；查询返回累计值。 */
static inline void lm_sched_stats_stuck_add(long n) {
    atomic_fetch_add_explicit(&g_lm_sched_stats.stuck_co_count, (uint64_t)n,
                              memory_order_relaxed);
}
static inline long lm_sched_stats_stuck_co(void) {
    return (long)atomic_load_explicit(&g_lm_sched_stats.stuck_co_count,
                                      memory_order_relaxed);
}

/* 记录一次 pending_time（ready→被执行，ns）。lm_co_resume 热点调用：
 * 桶判定为无界数组二分（12 桶），无锁原子累加。 */
void lm_sched_stats_record_pending_ns(uint64_t ns);

/* trace 线程懒启动（幂等）：解析 LM_SCHED_DEBUG=trace:N，每 N ms 聚合打印
 * 调度器全景到 stderr。N≤0 视为 1000ms。未设置环境变量则不启动。 */
void lm_sched_stats_start(void);

#ifdef __cplusplus
}
#endif

#endif /* LM_SCHED_STATS_H */
