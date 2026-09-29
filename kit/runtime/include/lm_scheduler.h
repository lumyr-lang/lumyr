// lm_scheduler.h —— per-thread 协程调度器（Phase 7.2 起；Phase 8.2 工作窃取升级）
// 对标 lthread 的 per-thread IO scheduler + Tokio/bthread/Go 的三层队列融合：
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
    lm_co_t* current;           /* 当前运行协程（reactor 主循环时 NULL） */
    lm_co_t* ready_head;        /* mutex 定向队列头（FIFO 单链，co->next 串联） */
    lm_co_t* ready_tail;        /* 定向队列尾 */
    pthread_mutex_t ready_mutex;/* 保护定向队列（跨线程投递） */
    int stop;                   /* 停止标志（scheduler_stop 置位） */
    /* Phase 7.4：compute worker 空闲等待 cond（加在末尾）。post 锁内 signal，
     * 无 waiter 时 no-op；IO scheduler 的 drain 由 reactor 驱动不用它（signal 无害）。 */
    pthread_cond_t idle_cond;
    /* ===== Phase 8.2：WSQ + LIFO slot + 窃取（末尾追加） ===== */
    lm_co_t* lifo_slot;         /* LIFO 单格快通道（同线程唤醒，对齐 Tokio） */
    int lifo_used;              /* 本轮 LIFO 已连续消费数（配额 LM_SCHED_LIFO_QUOTA/轮） */
    lm_wsq_t wsq;               /* 本地无锁工作窃取队列（owner push/pop） */
    pthread_t owner;            /* owner 线程标识（post_local 本线程判定） */
    uint32_t steal_seed;        /* 窃取随机起点种子（对齐 TaskControl _steal_seed） */
    uint32_t steal_offset;      /* 窃取质数步长（对齐 _steal_offset） */
    int sched_id;               /* 全局注册表槽位（-1 = 未注册，不参与窃取） */
    _Atomic int sleeping;       /* worker 阻塞睡眠标志（全局队列投递后唤醒扫描用） */
} lm_scheduler_t;

/* LIFO slot 每调度轮连续消费配额（对齐 Tokio MAX_LIFO_POLLS_PER_TICK=3，
 * worker.rs:269：防唤醒链 ping-pong 饿死队列其余任务） */
#define LM_SCHED_LIFO_QUOTA 3

/* 批量窃取单批上限（对齐 Go n-n/2 语义 + lumyr 协程迁移成本封顶） */
#define LM_SCHED_STEAL_MAX_BATCH 32

/* 创建 scheduler：绑定 reactor（可为 NULL，compute 池用）。
 * 创建即注册到全局注册表（参与窃取）；注册表满则 sched_id=-1（仍可用，
 * 只是不能被其他 scheduler 偷到）。失败返回 NULL。 */
lm_scheduler_t* lm_scheduler_new(lm_reactor_t* reactor);

/* 销毁 scheduler：先从全局注册表注销（防窃取者访问已释放结构），
 * 不销毁 reactor（reactor 由 caller 管），不销毁队列内协程
 * （协程由 owner 管，如 lm Coroutine 实例 destroy）。 */
void lm_scheduler_destroy(lm_scheduler_t* s);

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
 *      中立 → 本地 WSQ 尾（WSQ 满则抽后半段灌全局队列，对齐 Go runqputslow）。 */
void lm_scheduler_post_local(lm_scheduler_t* s, lm_co_t* co);

/* Phase 8.2：LIFO slot 投递（同线程唤醒快通道，对齐 Tokio schedule_local）：
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

/* Phase 7.4 + 8.2：阻塞取任务——compute worker 主循环用。
 * 顺序：mutex 定向队列 → 本地 WSQ → 全局队列取批灌 WSQ → 批量窃取
 * （随机起点 + 质数步长遍历注册表）→ 全空则 idle_cond 睡眠
 * （sleeping 标志置位，全局队列投递方扫描唤醒；8.3 起换 butex）。
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

#ifdef __cplusplus
}
#endif

#endif /* LM_SCHEDULER_H */
