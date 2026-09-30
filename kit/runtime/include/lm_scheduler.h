// lm_scheduler.h —— per-thread 协程调度器（Phase 7.2 起；Phase 8.2 工作窃取升级）
// per-thread IO scheduler + 三层队列融合：
//   LIFO slot（1 格，同线程唤醒快通道，配额 3/轮）
//   → mutex 定向队列（跨线程 wakeup / pinned / 非中立协程，保留 Phase 7.2 单链）
//   → 本地 WSQ（Chase-Lev 无锁，中立协程，可被窃取，见 lm_wsq.h）
//   → 全局溢出队列（带锁 injector，WSQ 满灌后半段 / 跨线程中立投递 / compute-in）
//   → 批量窃取（随机 victim + 质数步长，n=(len+1)/2 上限 32，重试 4）。
//
// 协程分流规则（post 路径，标志语义见 lm_co.h pinned/stealable 字段注释）：
//   - pinned || !stealable → 目标 scheduler 的 mutex 定向队列（永不入 WSQ）；
//   - 中立（!pinned && stealable）且本线程投递 → 本地 WSQ（满则抽后半段灌全局）；
//   - 中立且跨线程投递（compute-in 经 handle_migrate）→ 全局队列 + 唤醒空闲 worker。
//
// 线程模型：WSQ 的 push/pop 仅 owner 线程（scheduler 绑定线程），
// steal_batch 可被其他 scheduler 线程并发调用；mutex 定向队列任意线程可投递。
#ifndef LM_SCHEDULER_H
#define LM_SCHEDULER_H

#include "lm_reactor.h"
#include "lm_co.h"
#include "lm_wsq.h"
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lm_scheduler_s {
    lm_reactor_t* reactor;       /* 绑定 reactor（NULL = compute 池，Phase 7.4） */
    _Atomic(lm_co_t*) current;      /* 当前运行协程（reactor 主循环时 NULL）；sysmon 跨线程读，owner 写——原子化 */
    lm_co_t* ready_head;        /* mutex 定向队列头（FIFO 单链，co->next 串联） */
    lm_co_t* ready_tail;        /* 定向队列尾 */
    pthread_mutex_t ready_mutex;/* 保护定向队列（跨线程投递） */
    int stop;                   /* 停止标志（scheduler_stop 置位） */
    /* Phase 8.3：worker 睡眠改用 butex（替换 Phase 7.4 的 idle_cond）。
     * pop_blocking 在本字上 lm_butex_wait；post/overflow_wake_one 先改本字
     * 再 lm_butex_wake——无 waiter 时零系统调用（S4）。 */
    _Atomic uint32_t sleepWord;
    /* ===== Phase 8.2：WSQ + LIFO slot + 窃取（末尾追加） ===== */
    lm_co_t* lifo_slot;         /* LIFO 单格快通道（同线程唤醒） */
    int lifo_used;              /* 本轮 LIFO 已连续消费数（配额 LM_SCHED_LIFO_QUOTA/轮） */
    lm_wsq_t wsq;               /* 本地无锁工作窃取队列（owner push/pop） */
    pthread_t owner;            /* owner 线程标识（post_local 本线程判定） */
    uint32_t steal_seed;        /* 窃取随机起点种子 */
    uint32_t steal_offset;      /* 窃取质数步长 */
    int sched_id;               /* 全局注册表槽位（-1 = 未注册，不参与窃取） */
    _Atomic int sleeping;       /* worker 阻塞睡眠标志（全局队列投递后唤醒扫描用） */
    /* Phase 8.5 D：sysmon 带外监控字段。
     * schedtick：drain/pop_blocking 每轮 +1（调度心跳），
     *   sysmon 连续两轮快照相同 → 该 scheduler 卡在单个协程上。
     * tick_ns：最近一次 schedtick 变动的单调时间，判定是否超时用。 */
    _Atomic uint64_t schedtick;
    _Atomic uint64_t tick_ns;
    /* 异步唤醒源引用计数（timer 回调 vs destroyScheduler 竞态修复）：
     * 创建时 =1（owner 引用）；retain/release 配对；release 归零才真销毁。
     * TimerCbCtx 捕获 sched 时 retain、回调结束 release——回调在飞期间
     * destroyScheduler 只减不 free，post 不会踩已释放结构。 */
    _Atomic int refcnt;
    /* Phase 8.10：运行期缩容拒收标记（结构末尾追加，ABI 原则）。
     * shrink 置位后：round-robin 分发不再选中本 worker；worker 排空本地队列
     * 后自行退出（pop_blocking 返回 NULL 即队列空 → 见本标记 break）。 */
    _Atomic int reject_new;
    /* Phase 8.11：可观测性计数器（结构末尾追加，ABI 原则）。
     * 热路径无锁原子累加（per-thread 原子计数模式），LM_SCHED_DEBUG=trace:N 线程后台聚合打印。
     * steal_attempts/success：窃取尝试（每 victim 探测一次）/成功（批非空）次数。
     * wake_count：sched_wake_parked butex 唤醒次数。
     * lifo_quota_hits：LIFO 配额触顶次数（消费满 LM_SCHED_LIFO_QUOTA 且 slot 仍占）。
     * ready_len：mutex 定向队列当前深度（post/pop 持锁更新，trace 线程 relaxed 读）。 */
    _Atomic uint64_t steal_attempts;
    _Atomic uint64_t steal_success;
    _Atomic uint64_t wake_count;
    _Atomic uint64_t lifo_quota_hits;
    _Atomic long ready_len;
} lm_scheduler_t;

/* LIFO slot 每调度轮连续消费配额（LIFO 配额=3，
 * 防唤醒链 ping-pong 饿死队列其余任务） */
#define LM_SCHED_LIFO_QUOTA 3

/* 批量窃取单批上限（n-n/2 语义 + lumyr 协程迁移成本封顶） */
#define LM_SCHED_STEAL_MAX_BATCH 32

/* Phase 8.5：抢占与调度监控参数
 * LM_SCHED_LONG_SCHED_MS：drain 内长调度墙钟告警阈值（>此值打告警，不干预）。
 *   专抓"C 内建忘 BUMP_REDS"类事故；取较宽松 50ms 避免 C 内建正常长执行误报。
 * LM_SCHED_FORCE_MIGRATE_MS：sysmon 强制迁移阈值（10ms 强制抢占）。
 *   同 scheduler schedtick 超此时长未动 → 判定卡长协程，
 *   非 pinned 且 IO 线程的协程写 migrate_sched 迁到 compute 池。
 * LM_SCHED_DRAIN_GLOBAL_INTERVAL：drain 每 N 轮强制查全局溢出队列
 *   （schedtick%61==0），防全局队列饿死。
 * 三者均可编译期 -D 覆盖（验收测试用 10ms 口径对齐 S5 等条款）。 */
#ifndef LM_SCHED_LONG_SCHED_MS
#define LM_SCHED_LONG_SCHED_MS        50
#endif
#ifndef LM_SCHED_FORCE_MIGRATE_MS
#define LM_SCHED_FORCE_MIGRATE_MS     10
#endif
#ifndef LM_SCHED_DRAIN_GLOBAL_INTERVAL
#define LM_SCHED_DRAIN_GLOBAL_INTERVAL 61
#endif
/* Phase 8.14：drain 单次调用 resume 预算（仅 reactor 钩子路径生效，
 * s->reactor==NULL 的直接调用方保持"跑到队列空"旧语义）。
 * 根因：drain 原策略"直到队列空才返回"——slice_yield 即刻重入队的自旋
 * 协程（pinned 不可迁移 / 无 sysmon 时尤其）会把 drain 焊死在主循环
 * process_events 之前，kqueue/epoll 永不收事件，fd 等待全部退化到超时。
 * 预算耗尽且仍有就绪工作时，drain 写 reactor self-pipe 后返回：主循环
 * kevent/epoll_wait 被自唤醒字节立即带出（顺路收走已就绪 fd 事件），
 * 下一轮继续 drain——fd 事件采集间隔从"永不"变为 ≤每 61 次 resume，
 * 单个 kevent 立即返回的额外开销可忽略。 */
#ifndef LM_SCHED_DRAIN_BUDGET
#define LM_SCHED_DRAIN_BUDGET          61
#endif

/* 创建 scheduler：绑定 reactor（可为 NULL，compute 池用）。
 * 创建即注册到全局注册表（参与窃取）；注册表满则 sched_id=-1（仍可用，
 * 只是不能被其他 scheduler 偷到）。失败返回 NULL。 */
lm_scheduler_t* lm_scheduler_new(lm_reactor_t* reactor);

/* 销毁 scheduler（release 语义）：引用计数归零才真正释放——先从全局注册表
 * 注销（防窃取者访问已释放结构），不销毁 reactor（reactor 由 caller 管），
 * 不销毁队列内协程（协程由 owner 管，如 lm Coroutine 实例 destroy）。
 * 若有异步唤醒源（timer 回调）持引用，实际 free 推迟到最后一个 release。 */
void lm_scheduler_destroy(lm_scheduler_t* s);

/* 引用计数：异步唤醒源（timer 回调 ctx 等）在捕获 sched 指针前 retain、
 * 使用结束 release。retain/release 必须严格配对，否则泄漏或提前释放。 */
void lm_scheduler_retain(lm_scheduler_t* s);
void lm_scheduler_release(lm_scheduler_t* s);

/* 协程迁移/回家字段写入（引用计数版）：覆盖前 release 旧 scheduler、
 * retain 新 scheduler——协程存活期可能长于 scheduler 生命周期（timer 回调
 * spawn 的协程 post 到即将销毁的 sched 后仍可能被 sysmon 迁移/computeEnd
 * 回家），裸指针会在 handle_migrate/compute_worker 侧形成 UAF。 */
void lm_co_set_migrate_sched(struct lm_co_s* co, struct lm_scheduler_s* s);
void lm_co_set_home_sched(struct lm_co_s* co, struct lm_scheduler_s* s);
/* CAS 版：仅当 migrate_sched 当前为 NULL 才设为 s（成功 retain 并返回 1）。
 * sysmon 强制迁移用——与协程自身 computeBegin 并发写同一字段时，
 * 避免 check-then-set 双写导致 scheduler 引用泄漏/迁移目标错乱。 */
int lm_co_try_set_migrate_sched(struct lm_co_s* co, struct lm_scheduler_s* s);

/* TLS：设置/取当前线程 scheduler。scheduler 运行前置位，退出后清 NULL。
 * spawn/resume 路径通过 get_current 判断是否在 scheduler 上下文。
 * get_current 用 static inline 直接访问 _Thread_local（无函数调用帧）：
 * lm_co_resume 在协程嵌套 resume 热点调本函数判断投递，64KiB 协程栈
 * 嵌套 VM 调用本就紧张，内联避免额外栈帧顶到 guard page。 */
void lm_scheduler_set_current(lm_scheduler_t* s);

extern _Thread_local lm_scheduler_t* g_current_sched;

static inline lm_scheduler_t* lm_scheduler_get_current(void) {
    return g_current_sched;
}

/* 投递协程到 mutex 定向队列尾部（mutex 保护）。定向语义：协程必被
 * 本 scheduler 消费（wakeup/migrate/手动 postReady 统一入口）。
 * 设 co->queued=1 防重复入队；co->next 由队列消费时清。 */
void lm_scheduler_post(lm_scheduler_t* s, lm_co_t* co);

/* Phase 8.2：本地投递（仅 owner 线程调，spawn 自动入队用）。
 * 分流：pinned || !stealable → mutex 定向队列；
 *      中立 → 本地 WSQ 尾（WSQ 满则抽后半段灌全局队列，溢出协议）。 */
void lm_scheduler_post_local(lm_scheduler_t* s, lm_co_t* co);

/* Phase 8.2：LIFO slot 投递（同线程唤醒快通道，LIFO 快通道）：
 * slot 空则入 slot；slot 旧内容顶出——中立者压入本地 WSQ 尾，
 * 非中立/pinned 者转入 mutex 定向队列（WSQ 只放可被窃取的中立协程）。
 * 仅 owner 线程调用（同线程唤醒路径：channel send / cond signal 等）。 */
void lm_scheduler_post_lifo(lm_scheduler_t* s, lm_co_t* co);

/* Phase 7.3：跨线程唤醒——post 协程到 mutex 定向队列 + wakeup 目标 reactor。
 * 定向语义（Phase 8.2 起不走 WSQ 分流）：wakeup 的调用方已明确目标 scheduler
 * （fd 事件唤醒、computeEnd 回家、coWakeup 显式目标），协程必须回到该
 * scheduler 所在线程（栈数据亲和 / fd 亲和）。 */
void lm_scheduler_wakeup(lm_scheduler_t* s, lm_co_t* co);

/* 从 mutex 定向队列头取一个协程（mutex 保护）。
 * 清 co->queued=0、co->next=NULL。队列空返回 NULL。 */
lm_co_t* lm_scheduler_pop(lm_scheduler_t* s);

/* 消费就绪协程：drain 顺序 = LIFO slot（配额 3/轮）→ mutex 定向队列 →
 * 本地 WSQ pop。resume 前置 current=co（使 lm_co_resume 走直连路径），
 * yield 后清 current=NULL。resume 返回后调 handle_migrate 处理 compute 迁移。
 * DEAD 协程不自动销毁（由 owner 管，避免与 Coroutine.destroy 双重释放）。
 * IO scheduler 不在此处窃取/查全局队列（防计算任务卡 reactor）；
 * IO WSQ 内的中立协程由 compute worker 空闲时偷走。 */
void lm_scheduler_drain_ready(lm_scheduler_t* s);

/* Phase 7.4 + 8.2/8.3：阻塞取任务——compute worker 主循环用。
 * 顺序：mutex 定向队列 → 本地 WSQ → 全局队列取批灌 WSQ → 批量窃取
 * （随机起点 + 质数步长遍历注册表）→ 全空则 sleepWord 上 butex 睡眠
 * （sleeping 标志置位，全局队列投递方扫描唤醒）。
 * stop 置位且全源空时返回 NULL（worker 退出条件）。 */
lm_co_t* lm_scheduler_pop_blocking(lm_scheduler_t* s);

/* Phase 7.4：yield 后迁移处理——co->migrate_sched 非空时把协程投递到目标
 * scheduler。Phase 8.2 分流：目标为 compute scheduler（reactor=NULL）且协程
 * 中立（mig_copy 已备）→ 全局队列（池内负载均衡）+ 唤醒空闲 worker；
 * 否则（IO 回家 / pinned）→ wakeup 定向。
 * 必须在 lm_co_resume 返回后调（协程栈已让出，post 才安全——迁移协议核心）。 */
void lm_scheduler_handle_migrate(lm_co_t* co);

/* 停止 scheduler：置 stop 标志（reactor 由 caller 单独 stop）。 */
void lm_scheduler_stop(lm_scheduler_t* s);

/* ===== Phase 8.2：注册表与全局队列（内部状态，暴露查询供测试/监控） ===== */

/* 全局溢出队列深度快照（原子读，监控用）。 */
long lm_scheduler_overflow_len(void);

/* 已注册 scheduler 数（注册表快照）。 */
int lm_scheduler_registry_count(void);

/* Phase 8.5 D：sysmon 用——拷贝注册表快照到 out（最多 max 个）。
 * 返回实际拷贝数。持 g_scheds_mutex 拷贝，sysmon 遍历期间 scheduler 可能
 * 被注销（置 NULL 槽位），调用方需判空跳过。 */
int lm_scheduler_registry_snapshot(lm_scheduler_t** out, int max);

#ifdef __cplusplus
}
#endif

#endif /* LM_SCHEDULER_H */
