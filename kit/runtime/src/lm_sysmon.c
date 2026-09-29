// lm_sysmon.c —— Phase 8.5 D+E：sysmon 守护线程
// 对标 Go runtime sysmon（proc.go:6537）：带外扫描所有 scheduler，
// 检测卡在长协程上的 scheduler（schedtick 超时未动），触发强制迁移。
//
// 退避策略（对齐 Go proc.go:6548-6557）：
//   delay 起步 20µs；本扫描轮无超时 scheduler → delay *= 2；封顶 10ms。
//   有超时 → delay 重置为 20µs（快速响应）。
//
// 迁移策略（Phase 8.5 E，对齐 Go forcePreemptNS=10ms）：
//   scheduler 的 schedtick 连续两轮未变 且 now - tick_ns > LM_SCHED_FORCE_MIGRATE_MS
//   → 判定该 scheduler 卡在单个协程上。取其 current 协程：
//     - pinned（fd 绑定）→ 不迁（fd 亲和必须留在 IO 线程）。
//     - 非 pinned 且所在 scheduler 是 IO（reactor != NULL）→
//       写 co->migrate_sched = compute 池 scheduler，协程在下一个让出点
//       （含预算耗尽 LM_BUMP_REDS）被 drain/worker 经 handle_migrate 迁走。
//     - 已在 compute 池（reactor == NULL）→ 不迁（compute 池本就轮转，
//       且无 IO 可卡，长任务留池中是预期行为）。
#include "lm_sysmon.h"
#include "lm_scheduler.h"
#include "lm_co.h"
#include "lm_compute.h"
#include "lm_reactor.h"
#include "lm_butex.h"          /* Phase 8.13：lm_butex_check_waiter */
#include "lm_blocking_pool.h"  /* Phase 8.13：lm_blocking_pool_task_pending */
#include "lm_sched_stats.h"    /* Phase 8.13：stuck 计数 */
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LM_SYSMON_DELAY_MIN_NS   20000ULL     /* 20µs，对齐 Go sysmon 起步 */
#define LM_SYSMON_DELAY_MAX_NS   10000000ULL  /* 10ms，对齐 Go sysmon 封顶 */
#define LM_SYSMON_MAX_SCHEDS     256
/* Phase 8.13：stuck 协程扫描间隔（墙钟节流，复用 reaper 模式）。
 * 1s 一轮：检测精度不依赖超时猜测，间隔只影响确认延迟（两轮确认 ≈2s）。 */
#define LM_STUCK_SCAN_INTERVAL_NS  1000000000ULL  /* 1s */

static _Atomic int g_sysmon_started = 0;
static _Atomic int g_sysmon_stop = 0;
static pthread_t g_sysmon_thread;
static pthread_mutex_t g_sysmon_once_mutex = PTHREAD_MUTEX_INITIALIZER;

/* 单轮扫描：取注册表快照，检测卡住的 scheduler，执行强制迁移。
 * 返回本轮是否发现超时 scheduler（用于退避决策）。 */
static int sysmon_scan_once(uint64_t now_ns) {
    lm_scheduler_t* snap[LM_SYSMON_MAX_SCHEDS];
    int n = lm_scheduler_registry_snapshot(snap, LM_SYSMON_MAX_SCHEDS);
    int found_timeout = 0;
    uint64_t force_ns = (uint64_t)LM_SCHED_FORCE_MIGRATE_MS * 1000000ULL;

    for (int i = 0; i < n; i++) {
        lm_scheduler_t* s = snap[i];
        if (!s) continue;
        uint64_t tick_ns = atomic_load_explicit(&s->tick_ns, memory_order_relaxed);

        /* 边界防护：tick_ns==0（scheduler 尚未跑过 drain/pop）或时钟回退
         * （now_ns < tick_ns，跨线程读时序竞态）时跳过，避免下溢误报。 */
        if (tick_ns == 0 || now_ns < tick_ns) continue;

        /* 超时判定：tick_ns 是 drain/pop 每轮同步更新的（与 schedtick 同写）。
         * 若 scheduler 卡在单个协程上，drain 不返回，tick_ns 不再更新
         * → now - tick_ns 持续增长。超过 force_ns(10ms) 即判定卡住。
         * 等价于"schedtick 连续两轮未变"但更直接（tick_ns 自带墙钟语义）。
         * 注：scheduler 空闲（无协程）时 drain 也不运行，tick_ns 同样不更新，
         * 但此时 current==NULL，不会误触发迁移。 */
        if (now_ns - tick_ns <= force_ns) continue;

        /* 超时：取当前运行协程。current 为 NULL = 空闲，跳过。
         * current 由 owner 线程在 resume 前置位、resume 返回后清 NULL（原子写）。 */
        lm_co_t* co = atomic_load_explicit(&s->current, memory_order_acquire);
        if (!co) continue;

        found_timeout = 1;

        /* pinned 协程（fd 绑定）不迁——fd 亲和必须留在 IO 线程。 */
        if (co->pinned) continue;

        /* 已在 compute 池（reactor==NULL）不迁——compute 池本就轮转。 */
        if (s->reactor == NULL) continue;

        /* Phase 8.5 E：非 pinned + IO 线程卡死 → 强制迁到 compute 池。
         * 复用迁移协议：设 migrate_sched，协程在下一个让出点（LM_BUMP_REDS
         * 或自愿 yield）经 handle_migrate 迁走。
         * try_set（CAS 空→target）：若已设（上轮扫描设的，协程尚未让出消费；
         * 或协程并发 computeBegin 自设）不覆盖——防双写丢 scheduler 引用。 */
        lm_scheduler_t* target = lm_compute_pool_scheduler();
        if (target && lm_co_try_set_migrate_sched(co, target)) {
            fprintf(stderr,
                "[sysmon] force-migrate co=%p from io-sched=%p to compute-sched=%p "
                "(stuck %.2fms)\n",
                (void*)co, (void*)s, (void*)target,
                (now_ns - tick_ns) / 1000000.0);
        }
    }
    return found_timeout;
}

/* ============================================================
 * Phase 8.13：stuck 协程检测/救援（等待源注册表一致性校验）
 *
 * 原理：协程挂起时登记等待源类别（co->wait_kind）+ 等待字/目标 scheduler；
 * 本扫描校验「源状态 vs 协程状态」一致性——源侧已完成（fd 等待字终态已写 /
 * butex entry 已被摘队 / blocking 任务已不在册）而协程仍 SUSPENDED 且未入队
 * = 唤醒投递丢失（丢唤醒）。精确检测，零超时猜测：长 idle 连接挂起数小时
 * 源侧始终未完成，wait_kind 校验不疑似，不误报。
 *
 * 两轮确认（stuck_votes >= 2）：吸收「源侧刚完成、投递在飞」的 µs 级瞬态
 * 窗口（扫描间隔 1s，瞬态不可能跨两轮连续疑似）。
 *
 * 救援（仅重投安全的 case）：
 *   BUTEX：等待循环以 entry->woke==1 为退出条件、容忍伪唤醒，重投安全——
 *     entry 已摘队时 woke=1 已置，重投即真唤醒（协程退出循环正常续行）；
 *     目标 scheduler 取等待登记时 retain 的 co->waiting_sched（本扫描持
 *     注册表锁，与等待退出方 lm_co_release_waiting_sched 锁内 exchange
 *     互斥，retain 不会踩已释放 scheduler）。
 *   FD：终态已写时重投同样安全（等待循环重查等待字），但唤醒目标需经
 *     conn->sched 获取而 conn 可能已被复用——保守起见本期仅告警不救援。
 *   BLOCKING：await 路径无重查循环（yield 返回即读 result），重投不安全，
 *     仅告警。
 *
 * 遍历方式：lm_co_registry_foreach 持注册表锁（destroy 阻塞于锁，无快照
 * UAF 窗口；锁序 registry → butex 桶锁 / blocking 池锁 / scheduler ready
 * 锁，反向路径不存在，无死锁环）。
 * ============================================================ */
static void stuck_scan_co(lm_co_t* co, void* ctx) {
    (void)ctx;
    int kind = atomic_load_explicit(&co->wait_kind, memory_order_acquire);
    if (kind == LM_WAIT_NONE) return;
    /* 仅「挂起且不在任何就绪队列」的协程参与判定：RUNNING/READY = 已被唤醒
     * 或正在恢复；queued=1 = 投递已落地（等 drain），均非 stuck。 */
    if (atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_SUSPENDED ||
        atomic_load_explicit(&co->queued, memory_order_acquire) != 0) {
        atomic_store_explicit(&co->stuck_votes, 0, memory_order_relaxed);
        return;
    }
    int suspect = 0;
    lm_scheduler_t* rescue = NULL;
    switch (kind) {
    case LM_WAIT_FD: {
        _Atomic uintptr_t* w = atomic_load_explicit(&co->waiting_word,
                                                    memory_order_acquire);
        if (!w) break;   /* 与清除路径竞态的瞬态：下轮重判 */
        /* 挂起期间等待字恒为 WAITING(co) 或仲裁赢家写入的终态（READY/
         * TIMEOUT/CLOSED）——读到非 co 即终态已写，唤醒投递应已发生。 */
        if (atomic_load_explicit(w, memory_order_acquire) != (uintptr_t)co) {
            suspect = 1;
        }
        break;
    }
    case LM_WAIT_BUTEX: {
        _Atomic uintptr_t* w = atomic_load_explicit(&co->waiting_word,
                                                    memory_order_acquire);
        if (!w) break;
        /* entry 在表且 woke=0 = 正常等待（唤醒未发生，不疑似）；
         * 不在表 = entry 已被摘队（投递在飞瞬态 或 丢唤醒稳态，两轮确认）。 */
        if (lm_butex_check_waiter((volatile _Atomic uint32_t*)(void*)w, co) != 0) {
            suspect = 1;
            lm_scheduler_t* ws = atomic_load_explicit(&co->waiting_sched,
                                                      memory_order_acquire);
            if (ws) {
                lm_scheduler_retain(ws);   /* 注册表锁内，与释放方互斥，安全 */
                rescue = ws;
            }
        }
        break;
    }
    case LM_WAIT_BLOCKING: {
        /* 任务在册（队列中/执行中）= 回投尚未发生 = 正常等待。 */
        if (!lm_blocking_pool_task_pending(co)) suspect = 1;
        break;
    }
    default: break;
    }
    if (!suspect) {
        atomic_store_explicit(&co->stuck_votes, 0, memory_order_relaxed);
        return;
    }
    int votes = atomic_fetch_add_explicit(&co->stuck_votes, 1,
                                          memory_order_relaxed) + 1;
    if (votes < 2) return;   /* 首轮疑似：等下轮确认（吸收投递在飞瞬态） */
    fprintf(stderr,
        "[sysmon] stuck-co co=%p kind=%d votes=%d state=%d queued=%d "
        "（等待源已完成但协程未被唤醒，疑似丢唤醒）/ stuck coroutine detected\n",
        (void*)co, kind, votes,
        (int)atomic_load_explicit(&co->state, memory_order_acquire),
        atomic_load_explicit(&co->queued, memory_order_acquire));
    lm_sched_stats_stuck_add(1);
    /* 救援：仅 BUTEX 重投安全（语义见本段头注释）。确认后每轮都重投——
     * 重投生效则协程恢复并清除 wait_kind，下轮不再疑似；未生效（更深
     * 协议 bug）则持续告警+重投，无害。 */
    if (kind == LM_WAIT_BUTEX && rescue) {
        lm_scheduler_wakeup(rescue, co);
        fprintf(stderr,
            "[sysmon] stuck-co co=%p 已救援重投 / rescued via repost\n",
            (void*)co);
    }
    if (rescue) lm_scheduler_release(rescue);
}

static void stuck_scan_all(void) {
    lm_co_registry_foreach(stuck_scan_co, NULL);
}

static void* sysmon_thread_main(void* arg) {
    (void)arg;
    uint64_t delay_ns = LM_SYSMON_DELAY_MIN_NS;
    /* Phase 8.6 D：reaper 节流。sysmon 主循环 ~10ms 一轮（满载退避），
     * reaper 每 LM_REAP_SCAN_INTERVAL_NS(5s) 扫一次全局协程注册表换出 idle。
     * 用墙钟节流而非计数（delay 在 20µs~10ms 间变化，计数不稳定）。 */
    uint64_t last_reap_ns = 0;
    uint64_t last_stuck_ns = 0;   /* Phase 8.13：stuck 扫描节流（1s 一轮） */
    while (!atomic_load_explicit(&g_sysmon_stop, memory_order_acquire)) {
        uint64_t now = lm_now_ns();
        int timeout = sysmon_scan_once(now);
        if (timeout) {
            /* 有超时：快速响应，delay 重置到最小。 */
            delay_ns = LM_SYSMON_DELAY_MIN_NS;
        } else {
            /* 无超时：退避翻倍（封顶 10ms）。 */
            delay_ns *= 2;
            if (delay_ns > LM_SYSMON_DELAY_MAX_NS) delay_ns = LM_SYSMON_DELAY_MAX_NS;
        }
        /* Phase 8.6 D：idle 换出扫描（5s 一轮，扫描注册表换出 idle>30s 协程）。
         * reaper 持注册表锁遍历 + swap_out（GC+pool），与 scheduler 扫描解耦。
         * 首次 last_reap_ns==0 时跳过（等下一轮基线建立，防启动期误扫）。 */
        if (last_reap_ns != 0 && (now - last_reap_ns) >= LM_REAP_SCAN_INTERVAL_NS) {
            lm_co_reap_idle(now);
            last_reap_ns = now;
        } else if (last_reap_ns == 0) {
            last_reap_ns = now;
        }
        /* Phase 8.13：stuck 协程扫描（1s 一轮，等待源一致性校验 + 救援）。
         * 首次仅建基线跳过——防启动期协程大规模正常挂起被集中扫描
         *（虽不会误判，但启动瞬态扫描纯属浪费）。 */
        if (last_stuck_ns != 0 && (now - last_stuck_ns) >= LM_STUCK_SCAN_INTERVAL_NS) {
            stuck_scan_all();
            last_stuck_ns = now;
        } else if (last_stuck_ns == 0) {
            last_stuck_ns = now;
        }
        /* nanosleep 可被信号中断，此处不重试（短延迟，误差可接受）。 */
        struct timespec ts;
        ts.tv_sec = (time_t)(delay_ns / 1000000000ULL);
        ts.tv_nsec = (long)(delay_ns % 1000000000ULL);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

void lm_sysmon_start(void) {
    if (atomic_load_explicit(&g_sysmon_started, memory_order_acquire)) return;
    pthread_mutex_lock(&g_sysmon_once_mutex);
    if (g_sysmon_started) {
        pthread_mutex_unlock(&g_sysmon_once_mutex);
        return;
    }
    atomic_store_explicit(&g_sysmon_stop, 0, memory_order_release);
    if (pthread_create(&g_sysmon_thread, NULL, sysmon_thread_main, NULL) == 0) {
        atomic_store_explicit(&g_sysmon_started, 1, memory_order_release);
        atexit(lm_sysmon_stop);   /* 进程退出时干净停止（对齐 8.4 timer 线程模式） */
    }
    pthread_mutex_unlock(&g_sysmon_once_mutex);
}

void lm_sysmon_stop(void) {
    if (!atomic_load_explicit(&g_sysmon_started, memory_order_acquire)) return;
    atomic_store_explicit(&g_sysmon_stop, 1, memory_order_release);
    pthread_join(g_sysmon_thread, NULL);
    atomic_store_explicit(&g_sysmon_started, 0, memory_order_release);
}

int lm_sysmon_running(void) {
    return atomic_load_explicit(&g_sysmon_started, memory_order_acquire);
}
