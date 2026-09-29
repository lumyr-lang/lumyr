// lm_scheduler.c —— per-thread 协程调度器实现（Phase 7.2 起；Phase 8.2 工作窃取升级）
// 队列分层与分流规则见 lm_scheduler.h 头部设计说明。
// 本文件实现：TLS / 生命周期（含注册表注册注销）/ mutex 定向队列 / LIFO slot /
// 本地 WSQ 投递与溢出 / 全局溢出队列（injector）/ 批量窃取 / drain 与阻塞 pop。
#include "lm_scheduler.h"
#include "lm_butex.h"
#include "lm_reactor.h"   /* Phase 8.5：lm_now_ns() 长调度墙钟告警 */
#include "lm_sysmon.h"    /* Phase 8.5 D：sysmon 守护线程懒启动 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdio.h>

/* ============================================================
 * TLS：当前线程 scheduler 指针
 * 用 _Thread_local 而非 pthread_key：直接内存访问，无 pthread_getspecific
 * 调用帧。lm_co_resume 在协程嵌套 resume 热点调本函数判断是否投递就绪队列，
 * 用 _Thread_local 避免额外栈帧把协程栈（64KiB）顶到 guard page。
 * ============================================================ */

/* 外部链接的 _Thread_local：header 内 static inline lm_scheduler_get_current
 * 直接访问本变量，零调用帧。set_current 仍走函数（非热点，不在协程嵌套栈上）。 */
_Thread_local lm_scheduler_t* g_current_sched = NULL;

void lm_scheduler_set_current(lm_scheduler_t* s) {
    g_current_sched = s;
}

/* ============================================================
 * Phase 8.2：全局注册表 g_scheds[]（对齐 TaskControl _tagged_groups 机制）
 * scheduler 创建/销毁时注册/注销；窃取遍历以此为 victim 全集。
 * 生命周期事实：scheduler 仅在启动期创建、退出期销毁（ServiceApplication
 * onStart/onStop + compute 池），运行期不变——故窃取遍历持注册表锁
 * （窃取只在本地全空的冷路径发生，锁开销可忽略；注销在锁内置 NULL 槽位
 * 后才释放结构，窃取者不会访问已释放 scheduler）。
 * ============================================================ */
#define LM_MAX_SCHEDS 128

static lm_scheduler_t* g_scheds[LM_MAX_SCHEDS];
static int g_scheds_n = 0;
static pthread_mutex_t g_scheds_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint32_t g_scheds_version = 0;   /* 注册/注销递增（8.10 动态扩缩容预留） */

static void sched_registry_add(lm_scheduler_t* s) {
    pthread_mutex_lock(&g_scheds_mutex);
    s->sched_id = -1;
    /* 优先复用注销留下的 NULL 洞，保持数组紧凑 */
    for (int i = 0; i < g_scheds_n; i++) {
        if (!g_scheds[i]) {
            g_scheds[i] = s;
            s->sched_id = i;
            break;
        }
    }
    if (s->sched_id < 0 && g_scheds_n < LM_MAX_SCHEDS) {
        s->sched_id = g_scheds_n;
        g_scheds[g_scheds_n++] = s;
    }
    atomic_fetch_add_explicit(&g_scheds_version, 1, memory_order_release);
    pthread_mutex_unlock(&g_scheds_mutex);
}

static void sched_registry_remove(lm_scheduler_t* s) {
    pthread_mutex_lock(&g_scheds_mutex);
    if (s->sched_id >= 0 && s->sched_id < LM_MAX_SCHEDS &&
        g_scheds[s->sched_id] == s) {
        g_scheds[s->sched_id] = NULL;   /* 置 NULL 留洞：窃取者跳过，无 UAF */
    }
    s->sched_id = -1;
    atomic_fetch_add_explicit(&g_scheds_version, 1, memory_order_release);
    pthread_mutex_unlock(&g_scheds_mutex);
}

int lm_scheduler_registry_count(void) {
    pthread_mutex_lock(&g_scheds_mutex);
    int n = 0;
    for (int i = 0; i < g_scheds_n; i++) {
        if (g_scheds[i]) n++;
    }
    pthread_mutex_unlock(&g_scheds_mutex);
    return n;
}

/* Phase 8.5 D：sysmon 带外扫描用——持锁拷贝注册表指针快照。 */
int lm_scheduler_registry_snapshot(lm_scheduler_t** out, int max) {
    if (!out || max <= 0) return 0;
    pthread_mutex_lock(&g_scheds_mutex);
    int n = 0;
    for (int i = 0; i < g_scheds_n && n < max; i++) {
        if (g_scheds[i]) out[n++] = g_scheds[i];
    }
    pthread_mutex_unlock(&g_scheds_mutex);
    return n;
}

/* ============================================================
 * Phase 8.2：全局溢出队列（injector，对齐 Tokio inject/shared.rs）
 * 带锁单链 + 原子 len 旁路做无锁空检查——生产判断：全局队列本就该低频，
 * 一把 mutex 即可，不做无锁化（省自研风险）。
 * 内容保证：只有中立协程（!pinned && stealable）进入——
 *   WSQ 溢出抽后半段（WSQ 本就只放中立协程）+ compute-in 跨线程中立投递。
 * ============================================================ */
static lm_co_t* g_ov_head = NULL;
static lm_co_t* g_ov_tail = NULL;
static pthread_mutex_t g_ov_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic long g_ov_len = 0;

long lm_scheduler_overflow_len(void) {
    return atomic_load_explicit(&g_ov_len, memory_order_acquire);
}

/* Phase 8.3：唤醒一个已在 sleepWord 上 butex 睡眠的本 scheduler。
 * 协议：先 fetch_add 改字、后 butex_wake（丢唤醒防护）；worker 未睡时
 * 仅多一次字递增 + 桶内查找，无系统调用（S4）。 */
static void sched_wake_parked(lm_scheduler_t* s) {
    atomic_fetch_add_explicit(&s->sleepWord, 1, memory_order_release);
    lm_butex_wake(&s->sleepWord, 1);
}

/* 唤醒一个睡眠中的 compute worker（全局队列投递后）。
 * round-robin 起点扫描注册表找 sleeping 的 compute scheduler（reactor==NULL），
 * 唤醒其 sleepWord——固定顺序扫描会让注册表靠前的 worker 永远先醒先抢
 * 全局锁（压测实测独吞 60%），游标轮转对齐 Go wakep 的随机唤醒语义。
 * 找不到（大家都在忙）则跳过——忙碌 worker drain 时会查到全局队列。
 * 唤醒封顶 1 个（对齐 signal_task 封顶语义，防惊群）。 */
static void overflow_wake_one(void) {
    /* 游标由 g_scheds_mutex 保护（调用路径均经本函数，锁内串行递增） */
    static int g_wake_cursor = 0;
    lm_scheduler_t* target = NULL;
    pthread_mutex_lock(&g_scheds_mutex);
    int n = g_scheds_n;
    if (n > 0) {
        int start = g_wake_cursor++ % n;
        if (g_wake_cursor >= n * 1024) g_wake_cursor %= n;  /* 防 int 溢出 */
        for (int k = 0; k < n; k++) {
            lm_scheduler_t* s = g_scheds[(start + k) % n];
            if (s && !s->reactor &&
                atomic_load_explicit(&s->sleeping, memory_order_acquire)) {
                target = s;
                break;
            }
        }
    }
    pthread_mutex_unlock(&g_scheds_mutex);
    /* butex 唤醒放注册表锁外：不持全局锁做 syscall */
    if (target) sched_wake_parked(target);
}

/* 全局队列入队（置 queued=1）+ 唤醒一个空闲 worker。 */
static void overflow_push(lm_co_t* co) {
    co->queued = 1;
    co->next = NULL;
    pthread_mutex_lock(&g_ov_mutex);
    if (g_ov_tail) {
        g_ov_tail->next = co;
    } else {
        g_ov_head = co;
    }
    g_ov_tail = co;
    atomic_fetch_add_explicit(&g_ov_len, 1, memory_order_release);
    pthread_mutex_unlock(&g_ov_mutex);
    overflow_wake_one();
}

/* 从全局队列取一批（≤max_n 且不超过 WSQ 剩余容量）灌入本地 WSQ 尾部。
 * 对齐 Go runqget + Tokio inject→local 搬运语义。返回灌入个数。 */
static size_t overflow_take_to_wsq(lm_scheduler_t* s, size_t max_n) {
    if (atomic_load_explicit(&g_ov_len, memory_order_acquire) == 0) return 0;
    /* WSQ 剩余容量约束（灌入不能溢出本地） */
    int64_t b = atomic_load_explicit(&s->wsq.bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&s->wsq.top, memory_order_acquire);
    size_t room = s->wsq.capacity - (size_t)(b - t);
    if (room == 0) return 0;
    if (max_n > room) max_n = room;
    pthread_mutex_lock(&g_ov_mutex);
    size_t n = 0;
    while (n < max_n && g_ov_head) {
        lm_co_t* co = g_ov_head;
        g_ov_head = co->next;
        if (!g_ov_head) g_ov_tail = NULL;
        co->next = NULL;
        /* queued 保持 1：从全局队列搬入 WSQ，仍在"就绪队列"语义内 */
        lm_wsq_push(&s->wsq, co);   /* room 已预留，必成功 */
        n++;
    }
    atomic_fetch_sub_explicit(&g_ov_len, (long)n, memory_order_release);
    pthread_mutex_unlock(&g_ov_mutex);
    return n;
}

/* ============================================================
 * 生命周期
 * ============================================================ */

/* 窃取参数初始化：随机起点种子 + 质数步长（对齐 TaskControl 的
 * _steal_seed/_steal_offset：多 thief 从不同起点、以互质步长遍历，
 * 降低多 thief 同时撞同一 victim 的概率）。 */
static void sched_steal_param_init(lm_scheduler_t* s) {
    static const uint32_t primes[] = { 7, 13, 17, 19, 23, 29, 31, 37, 41, 43, 47, 53 };
    uint64_t mix = (uint64_t)(uintptr_t)s ^ (uint64_t)time(NULL) ^
                   ((uint64_t)(uintptr_t)pthread_self() << 16);
    s->steal_seed = (uint32_t)(mix ^ (mix >> 32));
    s->steal_offset = primes[mix % (sizeof(primes) / sizeof(primes[0]))];
}

lm_scheduler_t* lm_scheduler_new(lm_reactor_t* reactor) {
    lm_scheduler_t* s = (lm_scheduler_t*)calloc(1, sizeof(lm_scheduler_t));
    if (!s) return NULL;
    s->reactor = reactor;
    s->current = NULL;
    s->ready_head = NULL;
    s->ready_tail = NULL;
    pthread_mutex_init(&s->ready_mutex, NULL);
    atomic_store_explicit(&s->sleepWord, 0, memory_order_relaxed);
    s->stop = 0;
    /* Phase 8.2 字段 */
    s->lifo_slot = NULL;
    s->lifo_used = 0;
    if (lm_wsq_init(&s->wsq, 0) != 0) {
        pthread_mutex_destroy(&s->ready_mutex);
        free(s);
        return NULL;
    }
    s->owner = pthread_self();
    sched_steal_param_init(s);
    atomic_store_explicit(&s->sleeping, 0, memory_order_relaxed);
    /* Phase 8.5 D：schedtick/tick_ns 初始化（calloc 已零化 atomic，显式记时间）。 */
    atomic_store_explicit(&s->schedtick, 0, memory_order_relaxed);
    atomic_store_explicit(&s->tick_ns, lm_now_ns(), memory_order_relaxed);
    sched_registry_add(s);
    /* Phase 8.5 D：懒启动 sysmon 守护线程（幂等，首次 scheduler 创建时启动）。 */
    lm_sysmon_start();
    return s;
}

void lm_scheduler_destroy(lm_scheduler_t* s) {
    if (!s) return;
    /* 先从注册表注销（锁内置 NULL 槽位），再释放结构——
     * 窃取者持注册表锁遍历，注销后不会再拿到本 scheduler 指针。 */
    sched_registry_remove(s);
    /* 不销毁 reactor（caller 管）；不销毁队列内协程（owner 管）。
     * 队列内残留协程由 owner 自行 destroy，scheduler 仅释放自身结构。 */
    lm_wsq_destroy(&s->wsq);
    pthread_mutex_destroy(&s->ready_mutex);
    free(s);
}

/* ============================================================
 * mutex 定向队列（Phase 7.2 FIFO 单链，语义不变：定向、跨线程可投递）
 * ============================================================ */

void lm_scheduler_post(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    if (co->queued) return;   /* 防重复入队 */
    co->queued = 1;
    co->next = NULL;
    int needWake;
    pthread_mutex_lock(&s->ready_mutex);
    if (s->ready_tail) {
        s->ready_tail->next = co;
    } else {
        s->ready_head = co;
    }
    s->ready_tail = co;
    /* Phase 8.3：仅当本 worker 已在 butex 睡眠时需要唤醒。
     * sleeping 标志在 pop_blocking 持本锁置位，读取与入队同锁串行。 */
    needWake = atomic_load_explicit(&s->sleeping, memory_order_acquire);
    pthread_mutex_unlock(&s->ready_mutex);
    if (needWake) sched_wake_parked(s);
}

/* Phase 7.3：跨线程唤醒——post + wakeup reactor（定向语义，Phase 8.2 不分流）。
 * post 后写 reactor self-pipe 唤醒目标线程（mutex 保护 post 跨线程安全，
 * self-pipe 写非阻塞，管道满返回 EAGAIN 不阻塞调用方）。 */
void lm_scheduler_wakeup(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    lm_scheduler_post(s, co);
    if (s->reactor) lm_reactor_wakeup(s->reactor);
}

lm_co_t* lm_scheduler_pop(lm_scheduler_t* s) {
    if (!s) return NULL;
    pthread_mutex_lock(&s->ready_mutex);
    lm_co_t* co = s->ready_head;
    if (co) {
        s->ready_head = co->next;
        if (!s->ready_head) s->ready_tail = NULL;
        co->next = NULL;
        co->queued = 0;
    }
    pthread_mutex_unlock(&s->ready_mutex);
    return co;
}

/* ============================================================
 * Phase 8.2：本地投递分流（owner 线程）+ LIFO slot
 * ============================================================ */

/* WSQ push 满时的溢出处理：抽后半段灌全局（对齐 Go runqputslow），
 * 再重试 push 新协程。栈上分批抽取（批 256，防爆栈）。 */
static void wsq_push_or_overflow(lm_scheduler_t* s, lm_co_t* co) {
    if (lm_wsq_push(&s->wsq, co) == 0) return;
    void* batch[256];
    size_t n;
    while ((n = lm_wsq_extract_tail_half(&s->wsq, batch, 256)) > 0) {
        for (size_t i = 0; i < n; i++) {
            overflow_push(batch[i]);
        }
        if (lm_wsq_push(&s->wsq, co) == 0) return;
    }
    /* 极端：WSQ 仍满（队列被 pinned 占满不可能——WSQ 只收中立；
     * 唯一可能是 capacity 极小且全被并发 steal 锁定），兜底进全局 */
    if (lm_wsq_push(&s->wsq, co) != 0) {
        overflow_push(co);
    }
}

void lm_scheduler_post_local(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    if (co->queued) return;   /* 防重复入队 */
    /* 分流：pinned || !stealable → mutex 定向队列（栈数据/ fd 亲和本线程）；
     *      中立 → 本地 WSQ（可被窃取）。 */
    if (co->pinned || !co->stealable) {
        lm_scheduler_post(s, co);
        return;
    }
    co->queued = 1;
    wsq_push_or_overflow(s, co);
    /* WSQ 无锁对本线程 drain 立即可见；compute worker 睡眠时无法被
     * 本调用唤醒（owner 是 IO 线程时无需唤醒自己）——若本 scheduler 是
     * compute worker 且有空闲同伴，由同伴窃取，无需 signal。 */
}

void lm_scheduler_post_lifo(lm_scheduler_t* s, lm_co_t* co) {
    if (!s || !co) return;
    if (co->queued) return;
    co->queued = 1;
    lm_co_t* old = s->lifo_slot;
    s->lifo_slot = co;
    if (!old) return;
    /* 旧内容顶出：中立 → WSQ 尾；非中立/pinned → mutex 定向队列
     *（WSQ 只放中立协程，否则窃取者会偷走栈数据绑本线程的协程） */
    if (!old->pinned && old->stealable) {
        wsq_push_or_overflow(s, old);
    } else {
        /* 已置 queued=1 的 old 直接走定向入队（绕开 post 的 queued 检查） */
        old->next = NULL;
        int needWake;
        pthread_mutex_lock(&s->ready_mutex);
        if (s->ready_tail) {
            s->ready_tail->next = old;
        } else {
            s->ready_head = old;
        }
        s->ready_tail = old;
        needWake = atomic_load_explicit(&s->sleeping, memory_order_acquire);
        pthread_mutex_unlock(&s->ready_mutex);
        if (needWake) sched_wake_parked(s);
    }
}

/* ============================================================
 * Phase 8.2：批量窃取（对齐 Go runqgrab/runqsteal + bthread steal_task）
 * 随机起点（steal_seed）+ 质数步长（steal_offset）遍历注册表，
 * 每个 victim steal_batch（n=(len+1)/2 上限 LM_SCHED_STEAL_MAX_BATCH），
 * 批次灌本地 WSQ 尾部（溢出转全局），立即返回第一个执行。
 * ============================================================ */
static lm_co_t* sched_steal(lm_scheduler_t* self) {
    pthread_mutex_lock(&g_scheds_mutex);
    int n = g_scheds_n;
    if (n <= 1) {
        pthread_mutex_unlock(&g_scheds_mutex);
        return NULL;
    }
    lm_co_t* first = NULL;
    void* batch[LM_SCHED_STEAL_MAX_BATCH];
    uint32_t start = self->steal_seed % (uint32_t)n;
    for (int i = 0; i < n; i++) {
        uint32_t idx = (start + (uint32_t)i * self->steal_offset) % (uint32_t)n;
        lm_scheduler_t* v = g_scheds[idx];
        if (!v || v == self) continue;
        size_t got = lm_wsq_steal_batch(&v->wsq, batch, LM_SCHED_STEAL_MAX_BATCH);
        if (got == 0) continue;
        /* 灌本地 WSQ 尾部（对齐 runqsteal：灌一半、立即执行其一）；
         * 第一个不入队直接返回执行，减少一次 push/pop。 */
        for (size_t k = 1; k < got; k++) {
            wsq_push_or_overflow(self, batch[k]);
        }
        first = batch[0];
        first->queued = 0;   /* 直接执行，离开队列语义 */
        /* 推进种子：下次从不同起点开始（防同一 victim 反复被撞） */
        self->steal_seed = start + 1;
        break;
    }
    pthread_mutex_unlock(&g_scheds_mutex);
    return first;
}

/* ============================================================
 * 就绪队列消费：drain（IO scheduler，reactor 钩子每轮调用）
 * 顺序：LIFO slot（配额 3/轮）→ mutex 定向队列 → 本地 WSQ pop。
 * 不窃取、不查全局（IO 线程职责是 IO 调度，计算任务由 compute worker 偷）。
 * ============================================================ */

void lm_scheduler_drain_ready(lm_scheduler_t* s) {
    if (!s) return;
    s->lifo_used = 0;
    for (;;) {
        /* Phase 8.5 D：每轮 schedtick +1 + 记时间。sysmon 据此检测
         * scheduler 是否卡在单个协程上（连续两轮 schedtick 未变）。 */
        atomic_fetch_add_explicit(&s->schedtick, 1, memory_order_relaxed);
        atomic_store_explicit(&s->tick_ns, lm_now_ns(), memory_order_relaxed);
        lm_co_t* co = NULL;
        /* 1. LIFO slot（每轮配额 LM_SCHED_LIFO_QUOTA，防 ping-pong 饿死队列） */
        if (s->lifo_slot && s->lifo_used < LM_SCHED_LIFO_QUOTA) {
            co = s->lifo_slot;
            s->lifo_slot = NULL;
            s->lifo_used++;
            co->queued = 0;
        }
        /* 2. 本地 WSQ（可窃取协程：用户 handler / CPU 密集协程）
         * Phase 8.5：WSQ 提到 mutex 前——slice_yield 重入队走 mutex FIFO，
         * 让刚让出的 CPU 密集协程排到队尾，WSQ 中其他协程先跑，保证轮转公平。
         * pinned 协程（mutex）仍在 WSQ 空后立即运行，延迟仅一个 drain 周期。 */
        if (!co) {
            co = (lm_co_t*)lm_wsq_pop(&s->wsq);
            if (co) co->queued = 0;
        }
        /* 3. mutex 定向队列（pinned + slice_yield 重入队的 FIFO 轮转） */
        if (!co) co = lm_scheduler_pop(s);
        if (!co) break;
        /* 置 current=co：使协程内 lm_co_resume 走嵌套直连路径（同步切栈），
         * 而非再次投递到本队列（否则死循环）。yield 后清 current=NULL。 */
        s->current = co;
        lm_co_resume(co);
        s->current = NULL;
        /* Phase 8.5 C：长调度墙钟告警。协程单次 resume 墙钟耗时超过
         * LM_SCHED_LONG_SCHED_MS（默认 50ms）时打告警——通常意味着长 C 内建
         * 未主动让步（应调 LM_BUMP_ALL_REDS），或 sysmon 尚未介入。
         * 仅告警不干预（干预由 sysmon 强制迁移负责，子阶段 E）。 */
        {
            uint64_t elapsed_ns = lm_now_ns() - co->last_resume_ns;
            if (elapsed_ns > (uint64_t)LM_SCHED_LONG_SCHED_MS * 1000000ULL) {
                fprintf(stderr,
                    "[sched] long schedule: co=%p elapsed=%.2fms (threshold=%dms)\n",
                    (void*)co, elapsed_ns / 1000000.0, LM_SCHED_LONG_SCHED_MS);
            }
        }
        /* Phase 8.5：时间片耗尽让出重入队 vs 迁移——互斥。
         * 若 co->migrate_sched 非空（computeBegin / sysmon 强制迁移），
         * 由 handle_migrate 投递到目标 scheduler，不再重入队本 scheduler
         * （否则协程同时在两个队列 → 双线程同栈 UB）。
         * 仅当无迁移目标时，slice_yield 才重入队本 scheduler 继续轮转。 */
        if (co->slice_yield) {
            co->slice_yield = 0;
            if (co->state != LM_CO_DEAD && !co->migrate_sched) {
                /* 重入队到 mutex FIFO 队尾（非 WSQ LIFO），保证时间片轮转公平：
                 * 刚让出的 CPU 密集协程排到队尾，WSQ 中其他协程先跑。
                 * 用 lm_scheduler_post（mutex）而非 post_local（WSQ）。
                 * 不 break：drain 继续 pop 下一个协程（FIFO 轮转），直到队列空
                 * 才退出让 reactor 处理 fd/timer 事件。每片仅 4000 reds，fd 延迟
                 * 不超过一个时间片（微秒级），可接受。 */
                lm_scheduler_post(s, co);
            }
        }
        /* Phase 7.4 + 8.5 E：computeBegin / sysmon 强制迁移——投递到目标
         * scheduler（此时协程栈已让出，post 安全——迁移协议见 lm_co.h）。 */
        lm_scheduler_handle_migrate(co);
        /* DEAD 协程不自动销毁：避免与 lm Coroutine.destroy 双重释放。
         * owner（Coroutine 实例 / timer cb）负责 destroy。 */
    }
}

/* ============================================================
 * Phase 7.4 + 8.2：阻塞 pop——compute worker 主循环用。
 * 顺序：mutex 定向队列 → WSQ → 全局取批灌 WSQ → 窃取 → butex 睡眠。
 * 睡眠丢唤醒防护：sleeping 标志在 ready_mutex 内置位，recheck 覆盖全部
 * 任务源（ready_head + WSQ 近似长度 + 全局 len 原子）；唤醒方
 * （post / overflow_wake_one）"先改 sleepWord、后 butex_wake"。
 * ============================================================ */
lm_co_t* lm_scheduler_pop_blocking(lm_scheduler_t* s) {
    if (!s) return NULL;
    for (;;) {
        /* Phase 8.5 D：每轮 schedtick +1（同 drain_ready）。 */
        atomic_fetch_add_explicit(&s->schedtick, 1, memory_order_relaxed);
        atomic_store_explicit(&s->tick_ns, lm_now_ns(), memory_order_relaxed);
        /* 1. mutex 定向队列（非阻塞试） */
        lm_co_t* co = lm_scheduler_pop(s);
        if (co) return co;
        /* 2. 本地 WSQ */
        co = (lm_co_t*)lm_wsq_pop(&s->wsq);
        if (co) {
            co->queued = 0;
            return co;
        }
        /* 3. 全局队列取批灌 WSQ（取到则下轮循环 pop） */
        if (overflow_take_to_wsq(s, LM_SCHED_STEAL_MAX_BATCH) > 0) continue;
        /* 4. 批量窃取 */
        co = sched_steal(s);
        if (co) return co;
        /* 5. 全源空 → sleepWord 上 butex 睡眠（Phase 8.3）。
         * sleeping 置位 + recheck 持 ready_mutex，与 post 入队互斥；
         * expected 锁内读取——唤醒方改字必在 post 之后，若已 post 则
         * recheck 看到 ready_head 不会睡。wait 期间不持 ready_mutex，
         * 投递方不被睡眠 worker 阻塞。 */
        pthread_mutex_lock(&s->ready_mutex);
        atomic_store_explicit(&s->sleeping, 1, memory_order_release);
        while (!s->ready_head && !s->stop &&
               lm_wsq_size_approx(&s->wsq) == 0 &&
               atomic_load_explicit(&g_ov_len, memory_order_acquire) == 0) {
            uint32_t expected = atomic_load_explicit(&s->sleepWord,
                                                    memory_order_acquire);
            pthread_mutex_unlock(&s->ready_mutex);
            lm_butex_wait(&s->sleepWord, expected);
            pthread_mutex_lock(&s->ready_mutex);
        }
        atomic_store_explicit(&s->sleeping, 0, memory_order_release);
        co = s->ready_head;
        if (co) {
            s->ready_head = co->next;
            if (!s->ready_head) s->ready_tail = NULL;
            co->next = NULL;
            co->queued = 0;
        }
        int stopped = s->stop;
        pthread_mutex_unlock(&s->ready_mutex);
        if (co) return co;
        /* stop 且全源空：退出（返回 NULL 由 worker 主循环判断 break） */
        if (stopped &&
            lm_wsq_size_approx(&s->wsq) == 0 &&
            atomic_load_explicit(&g_ov_len, memory_order_acquire) == 0) {
            return NULL;
        }
        /* 被唤醒但无定向任务：WSQ/全局可能有货，循环重试 */
    }
}

/* Phase 7.4 + 8.2：yield 后迁移处理（IO drain 与 compute worker 循环统一调用）。
 * 分流（Phase 8.2）：
 *   目标为 compute scheduler（reactor=NULL）且协程中立 → 全局队列
 *   （池内负载均衡：任一空闲 worker 可取/偷，跨线程不能直 push 目标 WSQ）；
 *   否则（IO 回家 / pinned）→ wakeup 定向（self-pipe 唤醒 reactor /
 *   post 内 cond signal 唤醒 worker）。 */
void lm_scheduler_handle_migrate(lm_co_t* co) {
    if (!co || !co->migrate_sched) return;
    lm_scheduler_t* target = co->migrate_sched;
    co->migrate_sched = NULL;
    if (!target->reactor && !co->pinned && co->stealable) {
        overflow_push(co);
    } else {
        lm_scheduler_wakeup(target, co);
    }
}

void lm_scheduler_stop(lm_scheduler_t* s) {
    if (!s) return;
    /* 与 pop_blocking 睡眠段共用 ready_mutex 做握手互斥：二者各自先写
     * 自己的标志（stop / sleeping）再读对方标志，若无锁序列化，x86 TSO
     * 下双方可能互读对方 store buffer 里的旧值（stop 方读 sleeping=0
     * 不唤醒、睡眠方读 stop=0 入睡）→ 丢唤醒死锁。持锁后两种交错：
     *   stop 先拿锁 → 睡眠方锁内 recheck 到 stop 不睡；
     *   睡眠方先拿锁 → stop 看到 sleeping=1，锁外唤醒（睡眠方解锁后才
     *   真正 park，其 butex_wait 内部桶锁 recheck 覆盖“先改字后入队”）。
     * 唤醒放锁外，避免 futex 系统调用与嵌套桶锁压在 ready_mutex 内。 */
    pthread_mutex_lock(&s->ready_mutex);
    s->stop = 1;
    int wasSleeping = atomic_load_explicit(&s->sleeping,
                                           memory_order_acquire);
    pthread_mutex_unlock(&s->ready_mutex);
    /* 未睡时无系统调用（S4）；睡则唤醒让其 recheck stop 后退出 */
    if (wasSleeping) sched_wake_parked(s);
}
