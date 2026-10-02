// lm_signal.h —— 进程信号优雅退出（G1）
// nginx 信号表 + libuv self-pipe 范式：
//   - sigaction 表驱动注册（TERM/INT/HUP），处理器处于 async-signal-safe
//     约束下，仅做一次非阻塞 write 写 1 字节信号号到进程级信号管道；
//   - 信号管道读端经 lm_signal_attach 挂入 reactor 事件循环（普通 fd 监听），
//     可读时在 reactor 线程 drain 逐字节派发回调——.lm 回调协程由此在
//     事件循环线程运行，与 timer/coWakeup 同一调度路径；
//   - 进程内仅支持一个消费者 reactor（信号是进程级资源）。
#ifndef LM_SIGNAL_H
#define LM_SIGNAL_H

#include <signal.h>
#include "lm_reactor.h"   /* lm_reactor_t / lm_connection_t（attach 用） */

#ifdef __cplusplus
extern "C" {
#endif

/* 信号回调：reactor 事件循环线程调用（信号管道读事件 drain 内），
 * 非信号处理器上下文，可安全执行任意逻辑（含 spawn 协程）。 */
typedef void (*lm_signal_cb_t)(int signo, void* data);

/* 信号表按信号号直索引上限（标准信号 ≤ 31/32，管道 1 字节承载信号号） */
#define LM_SIGNAL_TABLE_SIZE 128

/* 初始化：创建进程级信号管道（非阻塞 + CLOEXEC，幂等）。成功 0，失败 -1 */
int lm_signal_init(void);

/* 注册信号处理（表驱动 sigaction，统一处理器仅写管道，幂等）。成功 0，失败 -1 */
int lm_signal_watch(int signo);

/* 把信号管道读端挂入 reactor 事件循环；可读时逐字节调 cb(signo, data)。
 * 进程内仅一个消费者（重复 attach 返回 -1）；必须在本 reactor owner 线程
 * 调用（reactor add 非线程安全）。成功 0，失败 -1 */
int lm_signal_attach(lm_reactor_t* r, lm_signal_cb_t cb, void* data);

/* 全部还原：已 watch 信号恢复 SIG_DFL + 关信号管道 + 清消费登记（幂等）。
 * 调用时机须在 reactor 事件循环退出之后（.lm 层 onStop 收尾保证）——
 * 管道读端仍挂在 reactor 后端上时关闭 fd 会留下失效注册 */
void lm_signal_shutdown(void);

/* 信号名 ↔ 信号号（"TERM"/"INT"/"HUP"）；未知返回 NULL / 0 */
const char* lm_signal_name(int signo);
int lm_signal_signo(const char* name);

#ifdef __cplusplus
}
#endif

#endif /* LM_SIGNAL_H */
