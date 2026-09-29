// lm_reactor.h —— 事件驱动 reactor（参考 nginx src/event）
// 单 reactor 线程跑所有协程；epoll/kqueue ET 后端；连接池 + 定时器最小堆 + posted 队列。
//
// 设计要点（对齐 nginx）：
//   - 后端抽象 lm_event_actions_t：add/del/enable/disable/process_events，
//     编译期 #if __linux__ 选 epoll、#elif __APPLE__ 选 kqueue，统一 ET 模式
//     （epoll EPOLLET；kqueue EV_CLEAR），与 nginx NGX_USE_CLEAR_EVENT 语义一致。
//   - 连接对象 lm_connection_t 池化（按 fd 索引 + free list 复用），
//     对齐 nginx ngx_cycle->connections / free_connections。
//   - 定时器最小堆（key=expire_ms），对齐 nginx ngx_event_timer（红黑树缓存友好变体）。
//   - posted_accept / posted_events 双队列，对齐 nginx ngx_posted_accept_events /
//     ngx_posted_events：避免事件回调内递归调用，把工作延后到主循环统一处理。
//
// 主循环顺序（对齐 nginx ngx_process_events_and_timers）：
//   process_events（epoll_wait/kevent，timeout=最近 timer 剩余）
//   → 处理 posted_accept → 过期 timer 派发 → 处理 posted_events。
#ifndef LM_REACTOR_H
#define LM_REACTOR_H

#include <stdint.h>
#include <stddef.h>
#include "lm_timer.h"   /* Phase 8.4：定时器集中化，add_timer/del_timer 转发 lm_timer_* */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 事件类型与回调
 * ============================================================ */

typedef enum {
    LM_EVENT_READ    = 0x01,   /* fd 可读 */
    LM_EVENT_WRITE   = 0x02,   /* fd 可写 */
    LM_EVENT_TIMEOUT = 0x04,   /* 定时器到期（连接级超时） */
    LM_EVENT_ERROR   = 0x08,   /* 错误（EPOLLERR/EPOLLHUP 等） */
    LM_EVENT_HUP     = 0x10,   /* 对端关闭/挂起 */
} lm_event_type_t;

/* reactor / connection 前向声明 */
typedef struct lm_reactor_s     lm_reactor_t;
typedef struct lm_connection_s  lm_connection_t;
/* Phase 8.4：conn->sched 指针（跨线程超时唤醒投递目标）。前向声明即可，
 * 不引 lm_scheduler.h 避免循环 include。 */
typedef struct lm_scheduler_s   lm_scheduler_t;

/* 事件回调：参数 (conn, events, data)。events 为 LM_EVENT_* 位掩码。
 * 在 reactor 主线程上下文执行；如需挂起协程，在回调内调 lm_co_yield。 */
typedef void (*lm_event_handler_t)(lm_connection_t* conn, uint32_t events, void* data);

/* 定时器回调：参数 (timer_id, data)。
 * Phase 8.4：回调在 timer 线程上下文执行（非 reactor 主线程），
 * MUST NOT block（见 lm_timer.h）。timer_id 为 lm_timer_id_t（64 位）。 */
typedef void (*lm_timer_cb_t)(lm_timer_id_t timer_id, void* data);

/* ============================================================
 * 连接对象（对齐 nginx ngx_connection_t）
 * ============================================================ */

#define LM_CONN_FLAG_ACTIVE  0x01   /* 已挂载到 reactor（add 成功后置位） */
#define LM_CONN_FLAG_READY   0x02   /* 已就绪待 posted 处理 */

struct lm_connection_s {
    int fd;                         /* 套接字 fd */
    lm_event_handler_t read_handler;
    lm_event_handler_t write_handler;
    void* read_data;                /* read_handler 的 data 参数 */
    void* write_data;               /* write_handler 的 data 参数 */
    uint32_t active_events;         /* 已挂载事件（LM_EVENT_READ/WRITE 位掩码） */
    uint32_t ready_events;         /* 就绪事件（process_events 写入，handler 读取） */
    uint32_t flags;
    int timedout;                   /* 定时器到期标记（连接级超时） */
    void* co;                       /* 所属协程指针（Phase 2 接入前置 NULL） */
    lm_scheduler_t* sched;          /* Phase 8.4：注册超时时所属 scheduler，
                                      * 超时回调（timer 线程）据此 wakeup 投递协程 */
    void* data;                     /* 用户自由数据 */
    lm_connection_t* next_free;     /* 连接池 free list 链 */
};

/* ============================================================
 * 后端抽象（对齐 nginx ngx_event_actions_t）
 * ============================================================ */

typedef struct {
    int  (*init)(lm_reactor_t* r);                                /* 创建 backend_fd */
    void (*fini)(lm_reactor_t* r);                                /* 关闭 backend_fd */
    int  (*add)(lm_reactor_t* r, lm_connection_t* c, uint32_t events);     /* 挂事件 */
    int  (*del)(lm_reactor_t* r, lm_connection_t* c, uint32_t events);     /* 摘事件 */
    int  (*enable)(lm_reactor_t* r, lm_connection_t* c, uint32_t events);  /* 启用（不挂） */
    int  (*disable)(lm_reactor_t* r, lm_connection_t* c, uint32_t events); /* 禁用（不摘） */
    int  (*process_events)(lm_reactor_t* r, int timeout_ms);               /* epoll_wait/kevent */
} lm_event_actions_t;

/* 平台后端实例（编译期选择）：
 * Linux → epoll_actions；macOS → kqueue_actions。在 lm_reactor.c 定义。 */
const lm_event_actions_t* lm_reactor_backend(void);

/* ============================================================
 * reactor 主对象
 * ============================================================ */

struct lm_reactor_s {
    int backend_fd;                       /* epoll_fd / kqfd */
    const lm_event_actions_t* actions;    /* 后端函数表 */
    /* 连接池：按 fd 索引；fd 超 capacity 时返回 NULL（用户需先扩容或拒绝） */
    lm_connection_t* connections;        /* conn_capacity 元数组 */
    int conn_capacity;
    lm_connection_t* free_conns;          /* free list 头（next_free 串联） */
    /* Phase 8.4：定时器集中到全局 TimerThread，reactor 不再维护最小堆。
     * 主循环只读 lm_timer_nearest_ms() 原子快照算 epoll_wait/kevent 超时。 */
    /* posted 队列：accept 优先级高于一般事件（对齐 nginx 双队列） */
    lm_connection_t** posted_accept;
    int posted_accept_count;
    int posted_accept_capacity;
    lm_connection_t** posted_events;
    int posted_events_count;
    int posted_events_capacity;
    volatile int stop_flag;
    int wake_pipe[2];   /* self-pipe：stop() 写 1 字节唤醒阻塞的 epoll_wait/kevent，
                          零延迟退出主循环；read 端挂 LM_EVENT_READ 由 wake_pipe_drain 清空 */
    /* Phase 7.2：scheduler 就绪队列消费钩子。
     * scheduler 绑定 reactor 时设本钩子，主循环每轮调一次消费就绪协程
     * （spawn 后的协程、reactor 事件唤醒的协程在此 resume）。
     * 无 scheduler 时为 NULL，主循环行为不变（既有单线程不破坏）。 */
    void (*ready_drain_cb)(void* data);
    void* ready_drain_data;
};

/* ============================================================
 * 公共 API
 * ============================================================ */

/* 创建 reactor：conn_capacity 为连接池大小（建议 ≥ 65536）。
 * 内部创建 backend_fd（epoll_create/kqueue），分配连接池 + 定时器堆 + posted 队列。
 * 失败返回 NULL。 */
lm_reactor_t* lm_reactor_new(int conn_capacity);
void lm_reactor_destroy(lm_reactor_t* r);

/* 取/归还连接对象（从连接池）：取按 fd 索引且置 fd；归还入 free list。
 * 取时连接对象字段未清零（用户自己初始化 read_handler/write_handler/data）。 */
lm_connection_t* lm_reactor_get_connection(lm_reactor_t* r, int fd);
void lm_reactor_free_connection(lm_reactor_t* r, lm_connection_t* c);

/* 挂/摘事件（调后端 add/del；同时更新 active_events 与 flags）。返回 0 成功。 */
int lm_reactor_add(lm_reactor_t* r, lm_connection_t* c, uint32_t events);
int lm_reactor_del(lm_reactor_t* r, lm_connection_t* c, uint32_t events);

/* posted 队列：把连接挂到 accept 队列或一般事件队列（去重：flag READY 防重复入队）。 */
void lm_reactor_post_accept(lm_reactor_t* r, lm_connection_t* c);
void lm_reactor_post_event(lm_reactor_t* r, lm_connection_t* c);

/* 定时器：转发到全局 TimerThread（Phase 8.4 集中化）。
 * expire_ms 为相对当前时间的过期毫秒数（如 5000 表示 5 秒后到期）。
 * 到期时 cb 在 timer 线程调用（MUST NOT block，见 lm_timer.h）。
 * 返回 timer_id（>0），失败 LM_TIMER_INVALID_ID（0）。
 * r 参保留兼容既有调用点签名，内部忽略（定时器全局共享）。 */
lm_timer_id_t lm_reactor_add_timer(lm_reactor_t* r, uint64_t expire_ms, lm_timer_cb_t cb, void* data);
void lm_reactor_del_timer(lm_reactor_t* r, lm_timer_id_t timer_id);

/* 主循环：阻塞至 stop_flag 或所有事件处理完。
 * 每轮：process_events(timeout=最近 timer 剩余) → 处理 posted_accept →
 *       过期 timer 派发 → 处理 posted_events。 */
void lm_reactor_run(lm_reactor_t* r);
void lm_reactor_stop(lm_reactor_t* r);

/* Phase 7.3：唤醒阻塞在 epoll_wait/kevent 的 reactor（不设 stop_flag）。
 * 跨线程唤醒场景（coWakeup/coCond signal）用：投递协程到目标 scheduler
 * 就绪队列后调本函数，目标线程 reactor 立即从 process_events 返回，
 * 下一轮 drain_ready 消费就绪队列 resume 协程。
 * 与 stop 的区别：stop 设 stop_flag 退出主循环；wakeup 仅唤醒继续循环。 */
void lm_reactor_wakeup(lm_reactor_t* r);

/* Phase 7.2：设/清 scheduler 就绪队列消费钩子。
 * scheduler 绑定 reactor 时调 set，主循环每轮调 cb(data) 消费就绪协程。
 * 清理时（scheduler destroy 前）调 set(NULL,NULL) 解绑，避免悬空。 */
void lm_reactor_set_ready_drain(lm_reactor_t* r, void (*cb)(void*), void* data);

/* 单调时钟（ms），与定时器 expire_ms 同基准。 */
uint64_t lm_reactor_now_ms(void);

/* Phase 8.5：单调时钟（ns），CLOCK_MONOTONIC。
 * 长调度墙钟告警 + sysmon schedtick 扫描用，精度高于 ms 版。 */
uint64_t lm_now_ns(void);

/* ============================================================
 * accept / connect helper（对齐 nginx ngx_event_accept / ngx_event_connect）
 * ============================================================ */

/* accept 一次：从 listener_conn->fd 非阻塞 accept 一个新连接。
 * 成功：返回新 lm_connection_t（已设 fd、read_handler=NULL 等），*out_addr 填客户端地址。
 * EAGAIN/EWOULDBLOCK：返回 NULL，*out_again=1（listener 应退出 accept 循环）。
 * EMFILE/ENFILE：返回 NULL，*out_again=0（连接数耗尽，跳过此次）。
 * 真错：返回 NULL，*out_again=0（建议 reactor stop）。
 * 新连接 fd 已自动设 NONBLOCK + CLOSE_ONEXEC。 */
lm_connection_t* lm_reactor_accept_one(lm_reactor_t* r, lm_connection_t* listener_conn,
                                        int* out_again);

/* 非阻塞 connect：fd 已设 NONBLOCK，调 connect()。
 * 返回 0：立即连上（如本地回环）。
 * 返回 LM_AGAIN (=1)：EINPROGRESS，挂写事件，可写时 reactor 调 write_handler
 *                     （handler 内调 lm_reactor_connect_check 判断成功/失败）。
 * 返回 -1：真错（errno 非 EINPROGRESS）。 */
#define LM_AGAIN 1
int lm_reactor_connect(lm_reactor_t* r, lm_connection_t* c);

/* connect 完成检查（写事件就绪后调）：getsockopt(SO_ERROR) 判断。
 * 返回 0：连接成功。返回 -1：连接失败（errno 在 *out_errno）。 */
int lm_reactor_connect_check(lm_connection_t* c, int* out_errno);

#ifdef __cplusplus
}
#endif

#endif /* LM_REACTOR_H */
