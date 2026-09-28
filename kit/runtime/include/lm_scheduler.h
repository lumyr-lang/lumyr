// lm_scheduler.h —— per-thread 协程调度器（Phase 7.2）
// 对标 lthread 的 per-thread IO scheduler：每线程一个 scheduler，绑定一个 reactor
// + 就绪协程队列（mutex 保护，支持跨线程投递，为 Phase 7.3 wakeup 打底）+ 当前协程。
//
// 设计要点：
//   - TLS scheduler 指针（对标 lthread_get_sched 的 pthread_getspecific），
//     spawn/resume 路径通过 lm_scheduler_get_current() 取当前线程 scheduler。
//   - 就绪队列：mutex 保护的 FIFO 单链（co->next 串联，co->queued 防重复入队）。
//     单线程下 mutex 无竞争（仅本线程投递 + 消费），开销可忽略；
//     多线程下 mutex 保证跨线程投递安全（Phase 7.3 coWakeup 投递到目标 scheduler）。
//   - reactor 钩子：reactor 主循环每轮调 lm_scheduler_drain_ready 消费就绪队列，
//     resume 每个就绪协程（跑到 yield 或结束）。spawn 后协程自动入队 → reactor
//     下一轮 drain 自动 resume，无需业务层显式 resume。
//   - current 协程：drain resume 前置 current=co，使 lm_co_resume 走直连路径
//     （嵌套 resume：协程内 resume 另一协程仍同步切栈），yield 后清 current=NULL。
//
// 行为兼容（既有单线程不破坏）：
//   - 无 scheduler 时 lm_co_resume 走原直连路径（C 测试、非 reactor 协程不受影响）。
//   - ServiceApplication 接入 scheduler 后，run() 结构不变（reactor.run 仍由 builtin
//     调，drain 由 reactor 钩子驱动），仅 onStart/onStop 建/毁 scheduler。
#ifndef LM_SCHEDULER_H
#define LM_SCHEDULER_H

#include "lm_reactor.h"
#include "lm_co.h"
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lm_scheduler_s {
    lm_reactor_t* reactor;       /* 绑定 reactor（NULL = compute 池，Phase 7.4） */
    lm_co_t* current;           /* 当前运行协程（reactor 主循环时 NULL） */
    lm_co_t* ready_head;        /* 就绪队列头（FIFO 单链，co->next 串联） */
    lm_co_t* ready_tail;        /* 就绪队列尾 */
    pthread_mutex_t ready_mutex;/* 保护就绪队列（跨线程投递） */
    int stop;                   /* 停止标志（scheduler_stop 置位） */
    /* Phase 7.4：compute worker 空闲等待 cond（加在末尾）。post 锁内 signal，
     * 无 waiter 时 no-op；IO scheduler 的 drain 由 reactor 驱动不用它（signal 无害）。 */
    pthread_cond_t idle_cond;
} lm_scheduler_t;

/* 创建 scheduler：绑定 reactor（可为 NULL，compute 池用）。失败返回 NULL。 */
lm_scheduler_t* lm_scheduler_new(lm_reactor_t* reactor);

/* 销毁 scheduler：不销毁 reactor（reactor 由 caller 管），不销毁队列内协程
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

/* 投递协程到就绪队列尾部（mutex 保护）。
 * 设 co->queued=1 防重复入队；co->next 由队列消费时清。 */
void lm_scheduler_post(lm_scheduler_t* s, lm_co_t* co);

/* Phase 7.3：跨线程唤醒——post 协程到就绪队列 + wakeup 目标 reactor。
 * 线程 A 调本函数把协程投递到线程 B 的 scheduler：post（mutex 保护）后
 * 写 reactor self-pipe 1 字节，线程 B 的 reactor 立即从 epoll_wait/kevent
 * 返回，下一轮 drain_ready resume 协程。无 reactor（compute 池）时仅 post
 * （worker 线程由 cond 唤醒，Phase 7.4）。 */
void lm_scheduler_wakeup(lm_scheduler_t* s, lm_co_t* co);

/* 从就绪队列头取一个协程（mutex 保护）。
 * 清 co->queued=0、co->next=NULL。队列空返回 NULL。 */
lm_co_t* lm_scheduler_pop(lm_scheduler_t* s);

/* 消费就绪队列：循环 pop + resume 直到空。
 * resume 前置 current=co（使 lm_co_resume 走直连路径），yield 后清 current=NULL。
 * resume 返回后调 handle_migrate 处理 compute 迁移（Phase 7.4）。
 * DEAD 协程不自动销毁（由 owner 管，避免与 Coroutine.destroy 双重释放）。 */
void lm_scheduler_drain_ready(lm_scheduler_t* s);

/* Phase 7.4：阻塞 pop——就绪队列空且未 stop 时 pthread_cond_wait 等待
 * （idle_cond，由 post 锁内 signal 唤醒）。compute worker 主循环用
 * （worker 无 reactor，不能靠 self-pipe）。stop 置位且队列空时返回 NULL
 * （worker 退出条件）。 */
lm_co_t* lm_scheduler_pop_blocking(lm_scheduler_t* s);

/* Phase 7.4：yield 后迁移处理——co->migrate_sched 非空时把协程 post 到目标
 * scheduler + 唤醒（有 reactor 写 self-pipe；无 reactor 由 post 内 cond signal）。
 * 必须在 lm_co_resume 返回后调（协程栈已让出，post 才安全——迁移协议核心）。
 * drain_ready（IO 线程 computeBegin 让出）与 compute worker 循环（computeEnd
 * 回家）统一走本函数。 */
void lm_scheduler_handle_migrate(lm_co_t* co);

/* 停止 scheduler：置 stop 标志（reactor 由 caller 单独 stop）。 */
void lm_scheduler_stop(lm_scheduler_t* s);

#ifdef __cplusplus
}
#endif

#endif /* LM_SCHEDULER_H */
