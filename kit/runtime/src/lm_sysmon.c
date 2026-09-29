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
#include <pthread.h>
#include <stdatomic.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LM_SYSMON_DELAY_MIN_NS   20000ULL     /* 20µs，对齐 Go sysmon 起步 */
#define LM_SYSMON_DELAY_MAX_NS   10000000ULL  /* 10ms，对齐 Go sysmon 封顶 */
#define LM_SYSMON_MAX_SCHEDS     256

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
         * current 由 owner 线程在 resume 前置位、resume 返回后清 NULL；
         * sysmon 跨线程读指针在 64 位平台对齐访问是原子的（x86/arm）。 */
        lm_co_t* co = s->current;
        if (!co) continue;

        found_timeout = 1;

        /* pinned 协程（fd 绑定）不迁——fd 亲和必须留在 IO 线程。 */
        if (co->pinned) continue;

        /* 已在 compute 池（reactor==NULL）不迁——compute 池本就轮转。 */
        if (s->reactor == NULL) continue;

        /* Phase 8.5 E：非 pinned + IO 线程卡死 → 强制迁到 compute 池。
         * 复用迁移协议：写 migrate_sched，协程在下一个让出点（LM_BUMP_REDS
         * 或自愿 yield）经 handle_migrate 迁走。
         * 仅当 migrate_sched 为空时才设——若已设（上轮扫描设的，协程尚未
         * 让出消费），不覆盖，避免 round-robin 换 target 导致协程迟迟不迁。 */
        if (!co->migrate_sched) {
            lm_scheduler_t* target = lm_compute_pool_scheduler();
            if (target) {
                co->migrate_sched = target;
                fprintf(stderr,
                    "[sysmon] force-migrate co=%p from io-sched=%p to compute-sched=%p "
                    "(stuck %.2fms)\n",
                    (void*)co, (void*)s, (void*)target,
                    (now_ns - tick_ns) / 1000000.0);
            }
        }
    }
    return found_timeout;
}

static void* sysmon_thread_main(void* arg) {
    (void)arg;
    uint64_t delay_ns = LM_SYSMON_DELAY_MIN_NS;
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
