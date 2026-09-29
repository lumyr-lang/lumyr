// lm_timer.c —— 集中化定时器线程实现（Phase 8.4）
// 全局单 TimerThread + 分桶 + 最小堆。借鉴 brpc bthread timer_thread.cpp。
// 设计依据与回调契约见 lm_timer.h。
//
// run 循环（对齐 timer_thread.cpp:340-514）：
//   reset snapshot=MAX → consume all buckets（merge 到本地堆）→
//   sweep 过期（CAS SCHEDULED→RUNNING，cb，回收）→
//   publish snapshot=heap top → futex_wait 到最近到期点。
#include "lm_timer.h"
#include "lm_butex.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>

/* ============================================================
 * Task 状态编码（version 单字段 CAS 防 ABA）
 *   version 低 2 位 = state，高 30 位 = seq（每次回收 +1）。
 *   state: 0=FREE, 1=SCHEDULED, 2=RUNNING, 3=DONE(取消/已执行)
 *   单字段 CAS 使 cancel 与 run 的状态转换原子无锁：
 *     cancel 只能 CAS SCHEDULED→DONE（若已是 RUNNING 则返回 1）；
 *     run   CAS SCHEDULED→RUNNING→回收(seq+1,FREE)。
 *   seq 递增防 ABA：slot 回收后 seq+1，旧 TaskId 的 seq 不匹配 → -1。
 * ============================================================ */
#define LM_TASK_ST_FREE   0u
#define LM_TASK_ST_SCHED  1u
#define LM_TASK_ST_RUN    2u
#define LM_TASK_ST_DONE   3u

#define TASK_VERSION(seq, st)  (((uint32_t)(seq) << 2) | (uint32_t)(st))
#define TASK_SEQ(v)           ((uint32_t)(v) >> 2)
#define TASK_STATE(v)         ((uint32_t)(v) & 0x3u)

typedef struct LmTimerTask {
    uint64_t run_time_ms;           /* 绝对单调 ms */
    lm_timer_fn_t fn;
    void* arg;
    uint32_t slot_index;             /* 在 pool->slots 中的下标 */
    _Atomic uint32_t version;        /* (seq<<2)|state */
    struct LmTimerTask* next;        /* 桶链 / free list 复用 */
} lm_timer_task_t;

/* ---- 最小堆（按 run_time_ms，数组实现）---- */
typedef struct {
    lm_timer_task_t** data;
    size_t size;
    size_t capacity;
} min_heap_t;

static void heap_push(min_heap_t* h, lm_timer_task_t* t) {
    if (h->size >= h->capacity) {
        size_t nc = h->capacity * 2 + 64;
        lm_timer_task_t** nd = (lm_timer_task_t**)realloc(h->data, nc * sizeof(*nd));
        if (!nd) return;   /* 扩容失败：丢弃该 task（rare，capacity 足够大时不会发生） */
        h->data = nd;
        h->capacity = nc;
    }
    size_t i = h->size++;
    h->data[i] = t;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (h->data[p]->run_time_ms <= h->data[i]->run_time_ms) break;
        lm_timer_task_t* tmp = h->data[p]; h->data[p] = h->data[i]; h->data[i] = tmp;
        i = p;
    }
}

static lm_timer_task_t* heap_pop(min_heap_t* h) {
    if (h->size == 0) return NULL;
    lm_timer_task_t* top = h->data[0];
    h->data[0] = h->data[--h->size];
    size_t i = 0;
    for (;;) {
        size_t l = 2 * i + 1, r = 2 * i + 2, s = i;
        if (l < h->size && h->data[l]->run_time_ms < h->data[s]->run_time_ms) s = l;
        if (r < h->size && h->data[r]->run_time_ms < h->data[s]->run_time_ms) s = r;
        if (s == i) break;
        lm_timer_task_t* tmp = h->data[s]; h->data[s] = h->data[i]; h->data[i] = tmp;
        i = s;
    }
    return top;
}

static inline lm_timer_task_t* heap_peek(const min_heap_t* h) {
    return h->size ? h->data[0] : NULL;
}

/* ---- 分桶（多生产者降低单堆 schedule 锁竞争）---- */
typedef struct {
    pthread_mutex_t mtx;
    uint64_t nearest_run_time;   /* 本桶最近到期（UINT64_MAX=空） */
    lm_timer_task_t* head;       /* 桶内单链（不排序，run consume 时统一建堆） */
} lm_timer_bucket_t;

/* ---- slot 池：Task 独立 malloc，slots 指针数组扩容不影响 Task 地址 ---- */
typedef struct {
    pthread_mutex_t mtx;
    lm_timer_task_t** slots;     /* capacity 个 Task* */
    size_t capacity;
    lm_timer_task_t* free_list;  /* next 串联 */
} lm_timer_pool_t;

static void pool_init(lm_timer_pool_t* p, size_t cap) {
    pthread_mutex_init(&p->mtx, NULL);
    p->capacity = cap;
    p->slots = (lm_timer_task_t**)calloc(cap, sizeof(lm_timer_task_t*));
    p->free_list = NULL;
    for (size_t i = 0; i < cap; i++) {
        lm_timer_task_t* t = (lm_timer_task_t*)malloc(sizeof(lm_timer_task_t));
        if (!t) continue;
        memset(t, 0, sizeof(*t));
        t->slot_index = (uint32_t)i;
        atomic_store_explicit(&t->version, TASK_VERSION(0, LM_TASK_ST_FREE), memory_order_relaxed);
        t->next = p->free_list;
        p->free_list = t;
        p->slots[i] = t;
    }
}

/* 扩容：补 GROW 个新 Task 到 free_list（slots realloc 只动指针数组，Task 地址不变） */
static int pool_grow(lm_timer_pool_t* p, size_t grow) {
    size_t nc = p->capacity + grow;
    lm_timer_task_t** nd = (lm_timer_task_t**)realloc(p->slots, nc * sizeof(*nd));
    if (!nd) return -1;
    p->slots = nd;
    for (size_t i = p->capacity; i < nc; i++) {
        lm_timer_task_t* t = (lm_timer_task_t*)malloc(sizeof(lm_timer_task_t));
        if (!t) { p->capacity = i; return -1; }
        memset(t, 0, sizeof(*t));
        t->slot_index = (uint32_t)i;
        atomic_store_explicit(&t->version, TASK_VERSION(0, LM_TASK_ST_FREE), memory_order_relaxed);
        t->next = p->free_list;
        p->free_list = t;
        p->slots[i] = t;
    }
    p->capacity = nc;
    return 0;
}

static lm_timer_task_t* pool_alloc(lm_timer_pool_t* p) {
    pthread_mutex_lock(&p->mtx);
    lm_timer_task_t* t = p->free_list;
    if (!t) {
        pool_grow(p, 256);
        t = p->free_list;
    }
    if (t) p->free_list = t->next;
    pthread_mutex_unlock(&p->mtx);
    return t;
}

static void pool_free(lm_timer_pool_t* p, lm_timer_task_t* t) {
    pthread_mutex_lock(&p->mtx);
    t->next = p->free_list;
    p->free_list = t;
    pthread_mutex_unlock(&p->mtx);
}

/* ============================================================
 * 全局 TimerThread 单例
 * ============================================================ */
typedef struct {
    lm_timer_bucket_t* buckets;
    size_t num_buckets;
    pthread_mutex_t mtx;            /* 保护 _nearest_run_time */
    uint64_t _nearest_run_time;     /* run 循环维护；add 比较判断是否 wake */
    _Atomic uint64_t nearest_snapshot; /* 原子快照，reactor 读 */
    _Atomic uint32_t nsignals;      /* futex 字，timer 线程睡在上面 */
    _Atomic int stop;
    pthread_t thread;
    _Atomic int started;   /* 0=未启动 1=已启动；原子读避免 add/cancel/nearest 持锁 */
    lm_timer_pool_t pool;
} lm_timer_thread_t;

static lm_timer_thread_t g_timer;
static pthread_mutex_t g_thread_mtx = PTHREAD_MUTEX_INITIALIZER;

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ============================================================
 * run 主循环（对齐 timer_thread.cpp:340-514）
 * ============================================================ */
static void* timer_run(void* arg) {
    lm_timer_thread_t* t = (lm_timer_thread_t*)arg;
    min_heap_t heap = { NULL, 0, 0 };

    while (!atomic_load_explicit(&t->stop, memory_order_acquire)) {
        /* 1. reset snapshot：consume 前先标空，reactor 此刻读到的最近到期
         *   会在 publish 后刷新。短暂窗口内 reactor 可能多 poll 一次（无 timer
         *   时 timeout cap 1s 兜底），不影响正确性。 */
        atomic_store_explicit(&t->nearest_snapshot, UINT64_MAX, memory_order_release);

        /* 1.2 nsignals 基线：必须在 consume 前取样（对齐 brpc timer_thread
         * run 循环协议）。若 add 的 fetch_add 发生在本轮 consume~publish 之间，
         * 基线不含该增量，步骤 4.5 复核发现差异即重跑 consume——关闭丢唤醒。 */
        uint32_t nsignals_base = atomic_load_explicit(&t->nsignals, memory_order_acquire);

        /* 2. consume all buckets：取每个桶 head 清空，reset bucket.nearest。
         *   桶内不排序，统一 push 进堆（最小堆保证 pop 顺序正确）。 */
        for (size_t i = 0; i < t->num_buckets; i++) {
            lm_timer_bucket_t* b = &t->buckets[i];
            pthread_mutex_lock(&b->mtx);
            lm_timer_task_t* head = b->head;
            b->head = NULL;
            b->nearest_run_time = UINT64_MAX;
            pthread_mutex_unlock(&b->mtx);

            while (head) {
                lm_timer_task_t* task = head;
                head = task->next;
                uint32_t v = atomic_load_explicit(&task->version, memory_order_acquire);
                if (TASK_STATE(v) == LM_TASK_ST_SCHED) {
                    heap_push(&heap, task);
                } else {
                    /* 已 cancel（DONE）：回收（seq+1 防 ABA） */
                    atomic_store_explicit(&task->version,
                        TASK_VERSION(TASK_SEQ(v) + 1, LM_TASK_ST_FREE), memory_order_release);
                    pool_free(&t->pool, task);
                }
            }
        }

        /* 3. sweep 过期：堆顶 <= now 则 pop 执行。 */
        uint64_t now = now_ms();
        while (heap.size && heap_peek(&heap)->run_time_ms <= now) {
            lm_timer_task_t* task = heap_pop(&heap);
            uint32_t cur = atomic_load_explicit(&task->version, memory_order_acquire);
            if (TASK_STATE(cur) != LM_TASK_ST_SCHED) {
                /* consume 后被 cancel（DONE）：回收 */
                atomic_store_explicit(&task->version,
                    TASK_VERSION(TASK_SEQ(cur) + 1, LM_TASK_ST_FREE), memory_order_release);
                pool_free(&t->pool, task);
                now = now_ms();
                continue;
            }
            /* CAS SCHEDULED→RUNNING：成功则执行 cb，失败说明 cancel 抢先改 DONE。 */
            uint32_t expected = cur;
            uint32_t desired = TASK_VERSION(TASK_SEQ(cur), LM_TASK_ST_RUN);
            if (atomic_compare_exchange_strong_explicit(&task->version, &expected, desired,
                    memory_order_acq_rel, memory_order_acquire)) {
                lm_timer_id_t id = ((uint64_t)cur << 32) | task->slot_index;
                task->fn(id, task->arg);
                /* 执行完回收：seq+1→FREE（防 ABA） */
                atomic_store_explicit(&task->version,
                    TASK_VERSION(TASK_SEQ(cur) + 1, LM_TASK_ST_FREE), memory_order_release);
                pool_free(&t->pool, task);
            } else {
                /* cancel 已改 DONE：回收 */
                atomic_store_explicit(&task->version,
                    TASK_VERSION(TASK_SEQ(cur) + 1, LM_TASK_ST_FREE), memory_order_release);
                pool_free(&t->pool, task);
            }
            now = now_ms();   /* cb 耗时后刷新 now */
        }

        /* 4. publish snapshot：堆顶即最近到期。空堆=UINT64_MAX。 */
        uint64_t nearest = heap.size ? heap_peek(&heap)->run_time_ms : UINT64_MAX;
        pthread_mutex_lock(&t->mtx);
        t->_nearest_run_time = nearest;
        pthread_mutex_unlock(&t->mtx);
        atomic_store_explicit(&t->nearest_snapshot, nearest, memory_order_release);

        /* 4.5 复核 nsignals：add 侧顺序是"先入桶后 fetch_add"，若增量落在
         * 基线取样之后、此处复核之前，说明本轮 consume 漏了新任务——重跑。
         * （丢唤醒根因：旧代码在此处现取 expected，若 add 的 fetch_add+wake
         * 已发生在 publish 与取样之间，futex 值匹配 → 空堆无限睡眠。
         * compute_test 首个 add 与线程懒启动首轮循环竞态确定性复现。） */
        if (atomic_load_explicit(&t->nsignals, memory_order_acquire) != nsignals_base) {
            continue;
        }

        /* 5. futex_wait 到最近到期点（或无限）。
         *   expected=基线值：add 若更早到期会 fetch_add nsignals + wake，
         *   使 futex_wait 值不匹配立即返回，下一轮 consume 新 task。
         *   值校验由 futex/ulock 原子完成（复核后、park 前落下的增量
         *   会使 *word != expected 而立即返回），无丢失窗口。 */
        uint32_t expected = nsignals_base;
        if (atomic_load_explicit(&t->stop, memory_order_acquire)) break;
        int64_t timeout_us = -1;   /* 无限 */
        if (nearest != UINT64_MAX) {
            uint64_t n = now_ms();
            if (nearest <= n) timeout_us = 0;
            else timeout_us = (int64_t)(nearest - n) * 1000;
        }
        lm_futex_wait(&t->nsignals, expected, timeout_us);
    }

    /* 收尾：丢弃堆内残留 task（进程退出，无须回调） */
    while (heap.size) {
        (void)heap_pop(&heap);
    }
    free(heap.data);
    return NULL;
}

/* ============================================================
 * 公共 API
 * ============================================================ */
int lm_timer_thread_start(size_t numBuckets) {
    pthread_mutex_lock(&g_thread_mtx);
    if (atomic_load_explicit(&g_timer.started, memory_order_acquire)) {
        pthread_mutex_unlock(&g_thread_mtx);
        return 0;
    }

    if (numBuckets == 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        numBuckets = (ncpu > 0) ? (size_t)ncpu : 4;
    }
    if (numBuckets < 4) numBuckets = 4;
    if (numBuckets > 1024) numBuckets = 1024;

    g_timer.buckets = (lm_timer_bucket_t*)calloc(numBuckets, sizeof(lm_timer_bucket_t));
    if (!g_timer.buckets) { pthread_mutex_unlock(&g_thread_mtx); return -1; }
    for (size_t i = 0; i < numBuckets; i++) {
        pthread_mutex_init(&g_timer.buckets[i].mtx, NULL);
        g_timer.buckets[i].nearest_run_time = UINT64_MAX;
        g_timer.buckets[i].head = NULL;
    }
    g_timer.num_buckets = numBuckets;
    pthread_mutex_init(&g_timer.mtx, NULL);
    g_timer._nearest_run_time = UINT64_MAX;
    atomic_store_explicit(&g_timer.nearest_snapshot, UINT64_MAX, memory_order_relaxed);
    atomic_store_explicit(&g_timer.nsignals, 0, memory_order_relaxed);
    atomic_store_explicit(&g_timer.stop, 0, memory_order_relaxed);
    pool_init(&g_timer.pool, 256);

    if (pthread_create(&g_timer.thread, NULL, timer_run, &g_timer) != 0) {
        for (size_t i = 0; i < numBuckets; i++) pthread_mutex_destroy(&g_timer.buckets[i].mtx);
        free(g_timer.buckets);
        g_timer.buckets = NULL;
        pthread_mutex_unlock(&g_thread_mtx);
        return -1;
    }
    atomic_store_explicit(&g_timer.started, 1, memory_order_release);
    /* atexit 注册停止：进程退出时回收 timer 线程（避免 join 不到的线程泄漏）。
     * stop 自身幂等（started 检查），重复注册安全。 */
    static int g_atexit_done = 0;
    if (!g_atexit_done) { g_atexit_done = 1; atexit(lm_timer_thread_stop); }
    pthread_mutex_unlock(&g_thread_mtx);
    return 0;
}

void lm_timer_thread_stop(void) {
    pthread_mutex_lock(&g_thread_mtx);
    if (!atomic_load_explicit(&g_timer.started, memory_order_acquire)) {
        pthread_mutex_unlock(&g_thread_mtx);
        return;
    }
    atomic_store_explicit(&g_timer.stop, 1, memory_order_release);
    /* wake timer 线程使其退出 futex_wait */
    atomic_fetch_add_explicit(&g_timer.nsignals, 1, memory_order_release);
    lm_futex_wake(&g_timer.nsignals);
    pthread_t th = g_timer.thread;
    pthread_mutex_unlock(&g_thread_mtx);
    pthread_join(th, NULL);

    pthread_mutex_lock(&g_thread_mtx);
    for (size_t i = 0; i < g_timer.num_buckets; i++) {
        pthread_mutex_destroy(&g_timer.buckets[i].mtx);
    }
    free(g_timer.buckets);
    g_timer.buckets = NULL;
    g_timer.num_buckets = 0;
    pthread_mutex_destroy(&g_timer.mtx);
    /* pool：释放所有 Task（slots 数组遍历） */
    for (size_t i = 0; i < g_timer.pool.capacity; i++) {
        free(g_timer.pool.slots[i]);
    }
    free(g_timer.pool.slots);
    pthread_mutex_destroy(&g_timer.pool.mtx);
    atomic_store_explicit(&g_timer.started, 0, memory_order_release);
    atomic_store_explicit(&g_timer.stop, 0, memory_order_release);
    pthread_mutex_unlock(&g_thread_mtx);
}

lm_timer_id_t lm_timer_add(uint64_t deadline_ms, lm_timer_fn_t fn, void* arg) {
    if (!fn) return LM_TIMER_INVALID_ID;
    /* 懒启动：首次 add 自动起 timer 线程（atexit 注册 stop 兜底回收）。
     * 避免调用方必须显式 start——reactor/ServiceApplication 启动顺序
     * 各异，懒启动使定时器随首个 add 就绪。 */
    if (!atomic_load_explicit(&g_timer.started, memory_order_acquire)) {
        if (lm_timer_thread_start(0) != 0) return LM_TIMER_INVALID_ID;
    }

    lm_timer_task_t* task = pool_alloc(&g_timer.pool);
    if (!task) return LM_TIMER_INVALID_ID;

    uint32_t old_v = atomic_load_explicit(&task->version, memory_order_acquire);
    /* free list 里 version = (seq<<2)|FREE(0)。分配时 state→SCHEDULED(1)，seq 不变。
     * TaskId 含 schedule 时的 version（state=SCHEDULED），cancel/run 用 seq 防护。 */
    uint32_t new_v = TASK_VERSION(TASK_SEQ(old_v), LM_TASK_ST_SCHED);
    atomic_store_explicit(&task->version, new_v, memory_order_release);
    task->run_time_ms = deadline_ms;
    task->fn = fn;
    task->arg = arg;
    lm_timer_id_t task_id = ((uint64_t)new_v << 32) | task->slot_index;

    /* 投入桶（pthread id hash % numBuckets，对齐 timer_thread 分桶） */
    size_t bi = (size_t)((uintptr_t)pthread_self() / sizeof(pthread_t)) % g_timer.num_buckets;
    lm_timer_bucket_t* b = &g_timer.buckets[bi];
    pthread_mutex_lock(&b->mtx);
    task->next = b->head;
    b->head = task;
    if (deadline_ms < b->nearest_run_time) b->nearest_run_time = deadline_ms;
    pthread_mutex_unlock(&b->mtx);

    /* 若早于 thread 最近到期 → wake timer 提前处理 */
    pthread_mutex_lock(&g_timer.mtx);
    int wake = (deadline_ms < g_timer._nearest_run_time);
    if (wake) g_timer._nearest_run_time = deadline_ms;
    pthread_mutex_unlock(&g_timer.mtx);
    /* nsignals 必须无条件递增（先入桶后递增，run 循环 4.5 复核协议依赖）。
     * 它是 run 循环的"任务变更"代计数：仅在 wake 时递增有一个丢唤醒窗口——
     * add 若落在 run 的 sweep~publish 之间（回调刚触发完、最近到期点尚未
     * 重算发布），_nearest_run_time 仍是触发前旧值（小于新任务到期点），
     * wake=0 不递增 → run 复核通过并 park 到旧最近到期点 → 新任务滞留桶中
     * 直到该旧到期点才被发现（compute_test 心跳链实测停摆 ~10s 至 bailout）。
     * 无条件递增后：落在 consume~复核间的 add 由复核发现（重跑 consume），
     * 落在复核~park 间的 add 由 futex 入口值校验发现（值不等立即返回）。
     * futex_wake 保持条件触发：线程已 park 且新任务晚于其睡眠目标时无需唤起
     *（其自身超时先到期，醒后 consume 自然取到新任务）。 */
    atomic_fetch_add_explicit(&g_timer.nsignals, 1, memory_order_release);
    if (wake) {
        lm_futex_wake(&g_timer.nsignals);
    }
    return task_id;
}

int lm_timer_cancel(lm_timer_id_t id) {
    if (id == LM_TIMER_INVALID_ID) return -1;
    if (!atomic_load_explicit(&g_timer.started, memory_order_acquire)) return -1;
    uint32_t id_version = (uint32_t)(id >> 32);
    uint32_t id_seq = TASK_SEQ(id_version);
    uint32_t slot = (uint32_t)(id & 0xFFFFFFFFu);

    pthread_mutex_lock(&g_timer.pool.mtx);
    if (slot >= g_timer.pool.capacity) {
        pthread_mutex_unlock(&g_timer.pool.mtx);
        return -1;
    }
    lm_timer_task_t* task = g_timer.pool.slots[slot];
    /* 不持锁读 version（原子）；但 CAS 用锁内串行保证 state 转换 */
    uint32_t cur = atomic_load_explicit(&task->version, memory_order_acquire);
    if (TASK_SEQ(cur) != id_seq) {
        pthread_mutex_unlock(&g_timer.pool.mtx);
        return -1;   /* ABA：slot 已回收复用 */
    }
    uint32_t st = TASK_STATE(cur);
    if (st == LM_TASK_ST_SCHED) {
        /* CAS SCHEDULED→DONE：用 expected 重试一次（run 可能抢先改 RUNNING） */
        uint32_t expected = cur;
        uint32_t desired = TASK_VERSION(TASK_SEQ(cur), LM_TASK_ST_DONE);
        if (atomic_compare_exchange_strong_explicit(&task->version, &expected, desired,
                memory_order_acq_rel, memory_order_acquire)) {
            pthread_mutex_unlock(&g_timer.pool.mtx);
            return 0;   /* cancelled */
        }
        /* CAS 失败：run 改了 RUNNING */
        pthread_mutex_unlock(&g_timer.pool.mtx);
        return 1;   /* running */
    }
    if (st == LM_TASK_ST_RUN) {
        pthread_mutex_unlock(&g_timer.pool.mtx);
        return 1;   /* running */
    }
    pthread_mutex_unlock(&g_timer.pool.mtx);
    return -1;   /* DONE（已取消/已执行） */
}

uint64_t lm_timer_nearest_ms(void) {
    if (!atomic_load_explicit(&g_timer.started, memory_order_acquire)) return UINT64_MAX;
    return atomic_load_explicit(&g_timer.nearest_snapshot, memory_order_acquire);
}
