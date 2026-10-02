// lm_reactor.c —— 事件驱动 reactor 实现
// 单 reactor 线程跑所有协程；epoll/kqueue ET 后端；连接池 + 定时器最小堆 + posted 队列。
//
// 主循环顺序：
//   process_events(timeout=最近 timer 剩余) → 处理 posted_accept →
//   过期 timer 派发 → 处理 posted_events → 检查 stop_flag。
//
// 后端抽象：
//   Linux → epoll（EPOLLET，ET 模式）
//   macOS → kqueue（EV_CLEAR，ET 模式）
//
// 连接池：按 fd 索引取，归还入 free list。fd 超 capacity 返回 NULL（用户拒绝或扩容）。
// 定时器最小堆：key=expire_ms（单调时钟）。堆顶最小，find_timer 取堆顶 expire。
// posted 队列：双队列（accept 优先 + 一般事件），避免事件回调内递归调用。
#include "lm_reactor.h"
#include "gc_runtime.h"
#include "lm_scheduler.h"   /* T5：入站投递唤醒 owner receiver（lm_scheduler_wakeup） */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <time.h>
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/timerfd.h>   /* 暂不用 timerfd，timer 用单调时钟 + 堆 */
#elif defined(__APPLE__)
#include <sys/event.h>   /* kqueue */
#endif

/* ============================================================
 * 时间基准（单调时钟，ms）
 * ============================================================ */

uint64_t lm_reactor_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

/* Phase 8.5：单调纳秒时钟（长调度告警 + sysmon 用）。 */
uint64_t lm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

/* ============================================================
 * self-pipe（stop() 即时唤醒机制）
 * ============================================================
 * 无 timer 且无就绪事件时 process_events 无限阻塞，stop_flag 仅在每轮顶部
 * 检查——stop() 经 self-pipe 写 1 字节让 epoll_wait/kevent 立即返回，
 * wake_pipe_drain 清空管道后顶部检查 stop_flag 退出主循环（零延迟）。
 * 注：STW 轮询仍由 lm_reactor_run 的 timeout cap（1s）承担——self-pipe
 * 不覆盖 GC STW（GC 不持有 reactor 的管道句柄，避免跨模块耦合）。 */

static int set_fd_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int set_fd_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

/* self-pipe 读回调：清空管道字节，消除 ET 模式可读状态。
 * stop() 写入的字节仅用于唤醒阻塞的 process_events，读后丢弃。
 * read 返回 EAGAIN 是正常态（未 stop 或已 drain 完），非错误。 */
static void wake_pipe_drain(lm_connection_t* conn, uint32_t events, void* data) {
    (void)events; (void)data;
    char buf[64];
    while (1) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n <= 0) break;  /* EAGAIN（已 drain 完）或 EOF */
    }
}

/* ============================================================
 * Linux epoll 后端
 * ============================================================ */

#ifdef __linux__

static int epoll_init(lm_reactor_t* r) {
    r->backend_fd = epoll_create1(EPOLL_CLOEXEC);
    return r->backend_fd >= 0 ? 0 : -1;
}

static void epoll_fini(lm_reactor_t* r) {
    if (r->backend_fd >= 0) { close(r->backend_fd); r->backend_fd = -1; }
}

/* 把 LM_EVENT_READ/WRITE 映射到 EPOLLIN/EPOLLOUT，统一 EPOLLET（ET 模式） */
static uint32_t epoll_map_events(uint32_t events) {
    uint32_t e = 0;
    if (events & LM_EVENT_READ)  e |= EPOLLIN;
    if (events & LM_EVENT_WRITE) e |= EPOLLOUT;
    return e | EPOLLET;   /* ET 模式 */
}

static int epoll_add(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = epoll_map_events(events);
    ev.data.ptr = c;     /* 直接挂 connection 指针 */
    return epoll_ctl(r->backend_fd, EPOLL_CTL_ADD, c->fd, &ev);
}

static int epoll_del(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    (void)events;
    /* EPOLL_CTL_DEL 忽略 events（删整个 fd 注册） */
    struct epoll_event ev;  /* 内核 < 2.6.9 需要 non-NULL，>= 2.6.9 可 NULL */
    memset(&ev, 0, sizeof(ev));
    return epoll_ctl(r->backend_fd, EPOLL_CTL_DEL, c->fd, &ev);
}

static int epoll_enable(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    (void)r; (void)c; (void)events;
    return 0;   /* epoll ET 模式无 enable/disable，仅 add/del */
}

static int epoll_disable(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    (void)r; (void)c; (void)events;
    return 0;
}

static int epoll_process_events(lm_reactor_t* r, int timeout_ms) {
    if (timeout_ms < 0) timeout_ms = -1;  /* -1 阻塞等 */
    if (timeout_ms > 0 && timeout_ms < 1) timeout_ms = 1;  /* 极短避免 0=非阻塞 */
    struct epoll_event events[256];
    int n;
    do {
        n = epoll_wait(r->backend_fd, events, 256, timeout_ms);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        lm_connection_t* c = (lm_connection_t*)events[i].data.ptr;
        if (!c) continue;
        uint32_t revents = 0;
        if (events[i].events & (EPOLLIN | EPOLLERR | EPOLLHUP)) revents |= LM_EVENT_READ;
        if (events[i].events & (EPOLLOUT | EPOLLERR | EPOLLHUP)) revents |= LM_EVENT_WRITE;
        if (events[i].events & EPOLLERR)  revents |= LM_EVENT_ERROR;
        if (events[i].events & EPOLLHUP) revents |= LM_EVENT_HUP;
        c->ready_events = revents;
        /* 直接调 handler（单线程 reactor，无并发）；posted 队列延后属另一路径 */
        if ((revents & (LM_EVENT_READ | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->read_handler) {
            c->read_handler(c, revents, c->read_data);
        }
        if ((revents & (LM_EVENT_WRITE | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->write_handler) {
            c->write_handler(c, revents, c->write_data);
        }
    }
    return n;
}

static const lm_event_actions_t epoll_actions = {
    epoll_init, epoll_fini,
    epoll_add, epoll_del,
    epoll_enable, epoll_disable,
    epoll_process_events
};

const lm_event_actions_t* lm_reactor_backend(void) {
    return &epoll_actions;
}

/* ============================================================
 * macOS kqueue 后端
 * ============================================================ */

#elif defined(__APPLE__)

static int kq_init(lm_reactor_t* r) {
    r->backend_fd = kqueue();
    if (r->backend_fd >= 0) {
        fcntl(r->backend_fd, F_SETFD, FD_CLOEXEC);
    }
    return r->backend_fd >= 0 ? 0 : -1;
}

static void kq_fini(lm_reactor_t* r) {
    if (r->backend_fd >= 0) { close(r->backend_fd); r->backend_fd = -1; }
}

/* kqueue EV_SET：filter=READ_FILTER/WRITE_FILTER，flags=EV_ADD|EV_CLEAR（ET） */
static int kq_apply(lm_reactor_t* r, lm_connection_t* c, uint32_t events, int add) {
    struct kevent changes[2];
    int nchg = 0;
    uint16_t flags = add ? (EV_ADD | EV_CLEAR | EV_EOF) : EV_DELETE;
    if (events & LM_EVENT_READ) {
        EV_SET(&changes[nchg++], c->fd, EVFILT_READ, flags, 0, 0, c);
    }
    if (events & LM_EVENT_WRITE) {
        EV_SET(&changes[nchg++], c->fd, EVFILT_WRITE, flags, 0, 0, c);
    }
    if (nchg == 0) return 0;
    return kevent(r->backend_fd, changes, nchg, NULL, 0, NULL);
}

static int kq_add(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    return kq_apply(r, c, events, 1);
}

static int kq_del(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    return kq_apply(r, c, events, 0);
}

static int kq_enable(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    (void)r; (void)c; (void)events;
    return 0;   /* kqueue EV_CLEAR 无 enable/disable */
}

static int kq_disable(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    (void)r; (void)c; (void)events;
    return 0;
}

static int kq_process_events(lm_reactor_t* r, int timeout_ms) {
    struct timespec ts;
    struct timespec* pts = NULL;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
        pts = &ts;
    }
    struct kevent events[256];
    int n;
    do {
        n = kevent(r->backend_fd, NULL, 0, events, 256, pts);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    for (int i = 0; i < n; i++) {
        lm_connection_t* c = (lm_connection_t*)events[i].udata;
        if (!c) continue;
        uint32_t revents = 0;
        if (events[i].filter == EVFILT_READ)  revents |= LM_EVENT_READ;
        if (events[i].filter == EVFILT_WRITE) revents |= LM_EVENT_WRITE;
        if (events[i].flags & EV_ERROR)  revents |= LM_EVENT_ERROR;
        if (events[i].flags & EV_EOF)    revents |= LM_EVENT_HUP;
        c->ready_events = revents;
        if ((revents & (LM_EVENT_READ | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->read_handler) {
            c->read_handler(c, revents, c->read_data);
        }
        if ((revents & (LM_EVENT_WRITE | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->write_handler) {
            c->write_handler(c, revents, c->write_data);
        }
    }
    return n;
}

static const lm_event_actions_t kq_actions = {
    kq_init, kq_fini,
    kq_add, kq_del,
    kq_enable, kq_disable,
    kq_process_events
};

const lm_event_actions_t* lm_reactor_backend(void) {
    return &kq_actions;
}

#else
#error "unsupported platform: only Linux (epoll) / macOS (kqueue) implemented"
#endif

/* ============================================================
 * 连接池
 * ============================================================ */

lm_connection_t* lm_reactor_get_connection(lm_reactor_t* r, int fd) {
    if (!r) return NULL;
    lm_connection_t* c = NULL;
    if (r->free_conns) {
        /* free list 复用 */
        c = r->free_conns;
        r->free_conns = c->next_free;
        c->next_free = NULL;
    } else if (fd >= 0 && fd < r->conn_capacity) {
        /* 首次按 fd 索引取（fd 索引直接定位） */
        c = &r->connections[fd];
    } else {
        return NULL;   /* 超 capacity，拒绝（用户应扩容或拒绝连接） */
    }
    /* 初始化字段（保留 fd 由 caller 设） */
    c->fd = fd;
    c->read_handler = NULL;
    c->write_handler = NULL;
    c->read_data = NULL;
    c->write_data = NULL;
    c->active_events = 0;
    c->ready_events = 0;
    c->flags = 0;
    c->timedout = 0;
    c->co = NULL;
    c->data = NULL;
    c->next_free = NULL;
    /* Phase 8.8：等待字归 NIL，fd_generation bump（slot 每次取用都是生命周期
     * 变更，使上一轮等待者/回调持有的 gen 快照立即失效） */
    atomic_store_explicit(&c->rg, LM_FD_NIL, memory_order_release);
    atomic_store_explicit(&c->wg, LM_FD_NIL, memory_order_release);
    atomic_fetch_add_explicit(&c->fd_generation, 1, memory_order_acq_rel);
    c->wait_gen_r = 0;
    c->wait_gen_w = 0;
    return c;
}

void lm_reactor_free_connection(lm_reactor_t* r, lm_connection_t* c) {
    if (!r || !c) return;
    /* 先从 reactor 摘事件（避免悬挂 fd 注册；fd 已被 close 时 del 返回
     * EBADF/ENOENT，忽略——内核在 close 时已自动摘除注册）。
     * 走 lm_reactor_del：内含 fd 易主守卫（Phase 8.14），fd 已被复用接管时
     * 绝不按 fd 发内核 del 误删新等待者注册。 */
    if (c->active_events) {
        (void)lm_reactor_del(r, c, c->active_events);
        c->active_events = 0;
        c->flags &= ~LM_CONN_FLAG_ACTIVE;
    }
    /* Phase 8.8：归还也是生命周期变更——bump gen 使残留回调的 wait_gen 快照
     * 失效；清除 fd 映射（仅当映射仍指向本 slot，防误删复用后的新登记） */
    atomic_fetch_add_explicit(&c->fd_generation, 1, memory_order_acq_rel);
    if (c->fd >= 0 && c->fd < r->conn_capacity && r->fd_map[c->fd] == c) {
        r->fd_map[c->fd] = NULL;
    }
    c->fd = -1;
    c->next_free = r->free_conns;
    r->free_conns = c;
}

/* ============================================================
 * 事件挂摘
 * ============================================================ */

int lm_reactor_add(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    if (!r || !c || !r->actions->add) return -1;
    if (r->actions->add(r, c, events) != 0) return -1;
    c->active_events |= events;
    c->flags |= LM_CONN_FLAG_ACTIVE;
    /* Phase 8.8：add 成功才登记 fd → conn 映射（close 路径据此找到等待中的
     * conn 做 WAITING→CLOSED 仲裁）。不在 get_connection 登记：add 失败的
     * 检出会回滚，避免短暂登记覆盖同 fd 既有等待者的映射。 */
    if (c->fd >= 0 && c->fd < r->conn_capacity) {
        r->fd_map[c->fd] = c;
    }
    return 0;
}

int lm_reactor_del(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    if (!r || !c || !r->actions->del) return -1;
    /* Phase 8.14：注册所有权守卫。fd_map 是"fd → 当前内核注册主人"的唯一
     * 事实表：add 成功时登记、free 时按指针相等清除、新 conn add 同 fd 覆盖。
     * 竞态场景：close(fd=N) 仲裁唤醒旧 waiter 后其 cleanup 排队滞后，
     * fd=N 被新 socket 立即复用、新 conn 完成 EV_ADD 并接管 fd_map[N]；
     * 旧 conn 之后才 cleanup。kqueue EV_DELETE / epoll EPOLL_CTL_DEL 只按
     * ident/fd 定位、不认 conn 指针——若照发会误删新等待者的注册，
     * 新等待者事件静默、只能退化到超时。
     * 检测到易主（fd_map 指向别人或已清空）→ 跳过内核 del，仅清本地记账。 */
    int fdOwned = (c->fd >= 0 && c->fd < r->conn_capacity &&
                   r->fd_map[c->fd] == c);
    if (!fdOwned) {
        c->active_events = 0;
        c->flags &= ~LM_CONN_FLAG_ACTIVE;
        return 0;
    }
    if (r->actions->del(r, c, events) != 0) return -1;
    c->active_events &= ~events;
    if (!c->active_events) c->flags &= ~LM_CONN_FLAG_ACTIVE;
    return 0;
}

/* ============================================================
 * posted 队列
 * ============================================================ */

void lm_reactor_post_accept(lm_reactor_t* r, lm_connection_t* c) {
    if (!r || !c) return;
    if (c->flags & LM_CONN_FLAG_READY) return;   /* 去重：已在队列 */
    c->flags |= LM_CONN_FLAG_READY;
    if (r->posted_accept_count >= r->posted_accept_capacity) {
        int new_cap = r->posted_accept_capacity * 2 + 64;
        lm_connection_t** p = (lm_connection_t**)realloc(
            r->posted_accept, (size_t)new_cap * sizeof(lm_connection_t*));
        if (!p) return;
        r->posted_accept = p;
        r->posted_accept_capacity = new_cap;
    }
    r->posted_accept[r->posted_accept_count++] = c;
}

void lm_reactor_post_event(lm_reactor_t* r, lm_connection_t* c) {
    if (!r || !c) return;
    if (c->flags & LM_CONN_FLAG_READY) return;
    c->flags |= LM_CONN_FLAG_READY;
    if (r->posted_events_count >= r->posted_events_capacity) {
        int new_cap = r->posted_events_capacity * 2 + 64;
        lm_connection_t** p = (lm_connection_t**)realloc(
            r->posted_events, (size_t)new_cap * sizeof(lm_connection_t*));
        if (!p) return;
        r->posted_events = p;
        r->posted_events_capacity = new_cap;
    }
    r->posted_events[r->posted_events_count++] = c;
}

/* 处理 posted 队列：取出每项调其 read_handler 或 write_handler（按 ready_events 分派）。
 * 处理时清除 READY 标志，允许回调内再次入队。 */
static void process_posted(lm_connection_t** queue, int* pcount) {
    int count = *pcount;
    *pcount = 0;
    for (int i = 0; i < count; i++) {
        lm_connection_t* c = queue[i];
        if (!c) continue;
        c->flags &= ~LM_CONN_FLAG_READY;
        uint32_t ev = c->ready_events;
        if ((ev & (LM_EVENT_READ | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->read_handler) {
            c->read_handler(c, ev, c->read_data);
        }
        if ((ev & (LM_EVENT_WRITE | LM_EVENT_ERROR | LM_EVENT_HUP)) && c->write_handler) {
            c->write_handler(c, ev, c->write_data);
        }
    }
}

/* ============================================================
 * 定时器（Phase 8.4：转发到全局 TimerThread）
 * reactor 不再维护最小堆，主循环只读 lm_timer_nearest_ms() 原子快照
 * 算 epoll_wait/kevent 超时；到期回调由 timer 线程直接执行并投递协程
 * 回归属 scheduler（见 lm_socket.c co_timeout_handler）。
 * ============================================================ */

lm_timer_id_t lm_reactor_add_timer(lm_reactor_t* r, uint64_t expire_ms, lm_timer_cb_t cb, void* data) {
    (void)r;   /* 定时器全局共享，r 参仅兼容既有调用点签名 */
    if (!cb) return LM_TIMER_INVALID_ID;
    /* lm_timer_cb_t 与 lm_timer_fn_t 签名一致 (id, data)，强转消除 typedef 差异 */
    return lm_timer_add(lm_reactor_now_ms() + expire_ms, (lm_timer_fn_t)cb, data);
}

void lm_reactor_del_timer(lm_reactor_t* r, lm_timer_id_t timer_id) {
    (void)r;
    lm_timer_cancel(timer_id);
}

/* ============================================================
 * reactor 主对象创建/销毁
 * ============================================================ */

lm_reactor_t* lm_reactor_new(int conn_capacity) {
    if (conn_capacity <= 0) conn_capacity = 65536;
    lm_reactor_t* r = (lm_reactor_t*)calloc(1, sizeof(lm_reactor_t));
    if (!r) return NULL;
    r->connections = (lm_connection_t*)calloc((size_t)conn_capacity, sizeof(lm_connection_t));
    r->fd_map = (lm_connection_t**)calloc((size_t)conn_capacity, sizeof(lm_connection_t*));
    r->posted_accept = (lm_connection_t**)calloc(64, sizeof(lm_connection_t*));
    r->posted_events = (lm_connection_t**)calloc(64, sizeof(lm_connection_t*));
    if (!r->connections || !r->fd_map || !r->posted_accept || !r->posted_events) {
        free(r->connections); free(r->fd_map);
        free(r->posted_accept); free(r->posted_events);
        free(r);
        return NULL;
    }
    r->conn_capacity = conn_capacity;
    r->posted_accept_capacity = 64;
    r->posted_accept_count = 0;
    r->posted_events_capacity = 64;
    r->posted_events_count = 0;
    r->stop_flag = 0;
    r->free_conns = NULL;
    /* 异步唤醒源引用计数：创建即持 owner 引用（=1）。scheduler 绑定时再
     * retain、销毁时 release；lm_reactor_destroy 释放本 owner 引用。
     * 不初始化则首个外部 retain/release 周期（如 T5 RR 选 worker）会
     * 错误归零并在 reactor 运行中释放（UAF）。 */
    atomic_store_explicit(&r->refcnt, 1, memory_order_relaxed);
    /* T5 RR：入站队列默认有界（env LM_INBOUND_CAP 可覆盖，仅测试/调优用）。
     * 正常背压下 acceptor 按负载选 worker，队列深度恒近 0；上限纯属安全网。 */
    r->inbound_head = NULL;
    r->inbound_tail = NULL;
    r->inbound_free = NULL;
    r->inbound_pending = 0;
    r->inbound_cap = 4096;
    r->reg_idx = -1;
    atomic_store_explicit(&r->inbound_load, 0, memory_order_relaxed);
    atomic_store_explicit(&r->inbound_reserved, 0, memory_order_relaxed);
    r->inbound_wait_co = NULL;
    r->inbound_wait_sched = NULL;
    {
        const char* env_cap = getenv("LM_INBOUND_CAP");
        if (env_cap && env_cap[0]) {
            int v = atoi(env_cap);
            if (v >= 1) r->inbound_cap = v;
        }
    }
    if (pthread_mutex_init(&r->inbound_mtx, NULL) != 0) {
        free(r->connections); free(r->fd_map);
        free(r->posted_accept); free(r->posted_events);
        free(r);
        return NULL;
    }
    /* 初始化连接池：所有节点都入 free list（next_free 串联，反向） */
    for (int i = conn_capacity - 1; i >= 0; i--) {
        r->connections[i].fd = -1;
        r->connections[i].next_free = r->free_conns;
        r->free_conns = &r->connections[i];
    }
    /* 创建后端 */
    r->actions = lm_reactor_backend();
    r->backend_fd = -1;
    if (r->actions->init(r) != 0) {
        free(r->connections); free(r->fd_map);
        free(r->posted_accept); free(r->posted_events);
        pthread_mutex_destroy(&r->inbound_mtx);
        free(r);
        return NULL;
    }
    /* self-pipe：read 端挂 LM_EVENT_READ 由 wake_pipe_drain 处理。
     * 失败不致命——回退为无即时唤醒（stop 依赖 timeout cap，延迟 ≤1s）。 */
    r->wake_pipe[0] = -1;
    r->wake_pipe[1] = -1;
    {
        int pfd[2];
        if (pipe(pfd) == 0) {
            set_fd_nonblock(pfd[0]);
            set_fd_nonblock(pfd[1]);
            set_fd_cloexec(pfd[0]);
            set_fd_cloexec(pfd[1]);
            lm_connection_t* wc = lm_reactor_get_connection(r, pfd[0]);
            if (wc && r->actions->add(r, wc, LM_EVENT_READ) == 0) {
                wc->read_handler = wake_pipe_drain;
                wc->read_data = NULL;
                wc->write_handler = NULL;
                wc->write_data = NULL;
                wc->co = NULL;
                r->wake_pipe[0] = pfd[0];
                r->wake_pipe[1] = pfd[1];
            } else {
                /* get_connection 失败（fd 超 capacity）或 add 失败：关管道回退 */
                close(pfd[0]);
                close(pfd[1]);
            }
        }
    }
    return r;
}

static void reactor_destroy_internal(lm_reactor_t* r) {
    if (r->actions && r->actions->fini) r->actions->fini(r);
    /* T5：残余入站 fd 全部在 owner 线程 close——shutdown 竞态下 acceptor 可能
     * 刚投递而 receiver 未及消费，这里兜底保证 fd 不泄漏（fd 守恒）。 */
    pthread_mutex_lock(&r->inbound_mtx);
    lm_inbound_t* node = r->inbound_head;
    while (node) {
        lm_inbound_t* nx = node->next;
        if (node->fd >= 0) close(node->fd);
        free(node);
        node = nx;
    }
    r->inbound_head = NULL;
    r->inbound_tail = NULL;
    node = r->inbound_free;
    while (node) {
        lm_inbound_t* nx = node->next;
        free(node);
        node = nx;
    }
    r->inbound_free = NULL;
    r->inbound_pending = 0;
    r->inbound_wait_co = NULL;
    r->inbound_wait_sched = NULL;
    pthread_mutex_unlock(&r->inbound_mtx);
    pthread_mutex_destroy(&r->inbound_mtx);
    /* fini 已关 backend_fd，wake read 端注册随之失效；关管道两端 */
    if (r->wake_pipe[0] >= 0) { close(r->wake_pipe[0]); r->wake_pipe[0] = -1; }
    if (r->wake_pipe[1] >= 0) { close(r->wake_pipe[1]); r->wake_pipe[1] = -1; }
    free(r->connections);
    free(r->fd_map);
    free(r->posted_accept);
    free(r->posted_events);
    free(r);
}

void lm_reactor_retain(lm_reactor_t* r) {
    if (!r) return;
    atomic_fetch_add_explicit(&r->refcnt, 1, memory_order_relaxed);
}

void lm_reactor_release(lm_reactor_t* r) {
    if (!r) return;
    /* acq_rel：归零线程看到此前所有持有者对结构的写；销毁与其余 release 串行。 */
    if (atomic_fetch_sub_explicit(&r->refcnt, 1, memory_order_acq_rel) == 1) {
        reactor_destroy_internal(r);
    }
}

void lm_reactor_destroy(lm_reactor_t* r) {
    /* release 语义：owner 引用减一；scheduler（及其异步唤醒源）持引用期间
     * 只减不 free，推迟到最后一个 release——修复 r.destroy() 与 timer 回调
     * 经 scheduler->reactor 写 self-pipe 并发的 use-after-free。
     * T5：注销必须先于最终 release——注销后 by_idx 立即取不到本 reactor，
     * 已在锁内 retain 到指针的提交者其 release 会把释放推迟到本调用之后。 */
    lm_reactor_unregister(r);
    lm_reactor_release(r);
}

void lm_reactor_stop(lm_reactor_t* r) {
    if (!r) return;
    r->stop_flag = 1;
    /* 写 1 字节唤醒阻塞在 epoll_wait/kevent 的主循环（self-pipe trick）。
     * 非阻塞写：管道满（上次 stop 字节未 drain）返回 EAGAIN，不阻塞调用方；
     * 写返回值忽略——stop_flag 已置位，唤醒意图靠 flag 保证，字节仅加速唤醒。 */
    if (r->wake_pipe[1] >= 0) {
        char c = 1;
        ssize_t n = write(r->wake_pipe[1], &c, 1);
        (void)n;
    }
}

/* Phase 7.3：唤醒 reactor（不设 stop_flag）。
 * 内部复用 stop 的 self-pipe 写逻辑：跨线程唤醒时目标 reactor 可能阻塞在
 * epoll_wait/kevent（无 timer 或无就绪事件），写 1 字节让它立即返回，
 * 下一轮 drain_ready 消费就绪队列。wake_pipe_drain 清空管道字节。 */
void lm_reactor_wakeup(lm_reactor_t* r) {
    if (!r) return;
    if (r->wake_pipe[1] >= 0) {
        char c = 1;
        ssize_t n = write(r->wake_pipe[1], &c, 1);
        (void)n;
    }
}

/* Phase 7.2：scheduler 就绪队列消费钩子设置/清理。
 * scheduler 绑定 reactor 时调 set 注册 drain 回调；解绑时调 set(NULL,NULL)。
 * 主循环每轮 process_events 后调 cb(data) 消费就绪协程。 */
void lm_reactor_set_ready_drain(lm_reactor_t* r, void (*cb)(void*), void* data) {
    if (!r) return;
    r->ready_drain_cb = cb;
    r->ready_drain_data = data;
}

/* ============================================================
 * 主循环
 * ============================================================ */

void lm_reactor_run(lm_reactor_t* r) {
    if (!r) return;
    while (!r->stop_flag) {
        /* 0. STW 安全点轮询：协程化后 VM 指令流由本循环驱动，
         * gc_stw_check 不再每条指令前必然执行（协程 yield 期间停滞）；
         * 每轮顶部显式轮询避免 STW 死锁（GC 等待 reactor 进入安全点）。
         * 非标记期仅一次 volatile 读 + 分支，无函数调用开销。 */
        gc_stw_check_fast();
        /* 1. 计算最近 timer 剩余作为 epoll_wait/kevent 超时。
         * Phase 8.4：timer 集中到全局 TimerThread，reactor 只读原子快照
         * lm_timer_nearest_ms()（UINT64_MAX=无 timer）。无 timer 时 cap 1s
         * 保证 STW 延迟 ≤1s（self-pipe 不覆盖 STW）。到期回调由 timer 线程
         * 直接执行并 wakeup 投递协程回本 reactor，无需本循环派发。 */
        uint64_t nearest = lm_timer_nearest_ms();
        int timeout_ms;
        if (nearest == UINT64_MAX) {
            timeout_ms = 1000;
        } else {
            uint64_t now = lm_reactor_now_ms();
            timeout_ms = (nearest <= now) ? 0 : (int)(nearest - now);
        }
        /* 1.5 Phase 7.2：process_events 前先消费 scheduler 就绪队列。
         * 关键时序：spawn 后的协程（如 accept loop）必须先 resume 注册 IO 事件，
         * 否则首轮 process_events 无事件可等会阻塞至 timeout cap（1s），首连接延迟。
         * drain 在前使协程注册事件 → process_events 立即能等到事件。
         * 无 scheduler 时 ready_drain_cb 为 NULL，开销仅一次分支判断。 */
        if (r->ready_drain_cb) r->ready_drain_cb(r->ready_drain_data);
        /* 2. 等待并派发就绪事件（handler 内可能调 reactor_post_*，下次处理） */
        r->actions->process_events(r, timeout_ms);
        /* 3. posted_accept 优先（accept 不能拖延，否则 listener ET 丢事件） */
        process_posted(r->posted_accept, &r->posted_accept_count);
        /* 4. posted_events（一般事件延后处理，避免回调递归）。
         * timer 过期派发已移除（由 timer 线程直接执行回调）。 */
        process_posted(r->posted_events, &r->posted_events_count);
    }
}

/* ============================================================
 * accept helper
 * ============================================================ */

lm_connection_t* lm_reactor_accept_one(lm_reactor_t* r, lm_connection_t* listener_conn,
                                        int* out_again) {
    if (!r || !listener_conn || !out_again) return NULL;
    *out_again = 0;
    struct sockaddr_storage cli;
    socklen_t clilen = sizeof(cli);
    int cfd;
#ifdef __linux__
    /* Linux accept4 一步到位：NONBLOCK + CLOEXEC */
    cfd = accept4(listener_conn->fd, (struct sockaddr*)&cli, &clilen, SOCK_NONBLOCK | SOCK_CLOEXEC);
#else
    /* macOS/BSD accept + fcntl NONBLOCK + CLOEXEC */
    cfd = accept(listener_conn->fd, (struct sockaddr*)&cli, &clilen);
    if (cfd >= 0) {
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl != -1) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(cfd, F_GETFD, 0);
        if (fd != -1) fcntl(cfd, F_SETFD, fd | FD_CLOEXEC);
    }
#endif
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            *out_again = 1;   /* ET 模式无更多连接，退出 accept 循环 */
            return NULL;
        }
        if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) {
            *out_again = 0;   /* 进程级 fd 耗尽，跳过此次 */
            return NULL;
        }
        /* EINTR 也算可重试，但 ET 模式下重试 accept 可能立即再次 EINTR；返回不重试 */
        if (errno == EINTR) { *out_again = 1; return NULL; }
        *out_again = 0;   /* 真错，建议 listener 关闭或 reactor stop */
        return NULL;
    }
    /* fd 超连接池容量：直接关闭拒绝（避免越界） */
    if (cfd >= r->conn_capacity) {
        close(cfd);
        *out_again = 0;
        return NULL;
    }
    lm_connection_t* c = lm_reactor_get_connection(r, cfd);
    if (!c) {
        close(cfd);
        *out_again = 0;
        return NULL;
    }
    return c;
}

/* ============================================================
 * connect helper
 * ============================================================ */

int lm_reactor_connect(lm_reactor_t* r, lm_connection_t* c) {
    if (!r || !c || c->fd < 0) return -1;
    /* 假定 caller 已设 fd NONBLOCK + 已调 connect()（addr 在 caller 设）。
     * 此 helper 仅判断 connect 结果是否 EINPROGRESS；如非 EINPROGRESS 视为立即成功/失败，
     * EINPROGRESS 挂写事件由 caller 自行处理。 */
    (void)r;
    return LM_AGAIN;
}

int lm_reactor_connect_check(lm_connection_t* c, int* out_errno) {
    if (!c || c->fd < 0) return -1;
    int err = 0;
    socklen_t errlen = sizeof(err);
    if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0) {
        if (out_errno) *out_errno = errno;
        return -1;
    }
    if (err != 0) {
        if (out_errno) *out_errno = err;
        return -1;
    }
    return 0;
}

/* ============================================================
 * T5：reactor 注册表（RR acceptor 按 worker idx 投递）
 * ============================================================
 * 与 g_scheds 同构但独立：RR 交接的载荷是裸 fd、目标是 reactor 本身，
 * 不经过 scheduler。槽位按 worker idx 显式占用（注册顺序与 worker 创建
 * 顺序无竞态）；by_idx 在锁内 retain，注销先于 reactor 最终 release，
 * 跨线程提交期间 reactor 不可能被 free。 */

static lm_reactor_t* g_reactors[LM_REACTOR_MAX_REG];
static pthread_mutex_t g_reactors_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_reactors_count = 0;

int lm_reactor_register(lm_reactor_t* r, int idx) {
    if (!r || idx < 0 || idx >= LM_REACTOR_MAX_REG) return -1;
    pthread_mutex_lock(&g_reactors_mutex);
    if (r->reg_idx >= 0 || g_reactors[idx] != NULL) {
        pthread_mutex_unlock(&g_reactors_mutex);
        return -1;
    }
    r->reg_idx = idx;
    g_reactors[idx] = r;
    atomic_fetch_add_explicit(&g_reactors_count, 1, memory_order_acq_rel);
    pthread_mutex_unlock(&g_reactors_mutex);
    return 0;
}

void lm_reactor_unregister(lm_reactor_t* r) {
    if (!r || r->reg_idx < 0) return;
    pthread_mutex_lock(&g_reactors_mutex);
    if (r->reg_idx < LM_REACTOR_MAX_REG && g_reactors[r->reg_idx] == r) {
        g_reactors[r->reg_idx] = NULL;
        atomic_fetch_sub_explicit(&g_reactors_count, 1, memory_order_acq_rel);
    }
    r->reg_idx = -1;
    pthread_mutex_unlock(&g_reactors_mutex);
}

int lm_reactor_registry_count(void) {
    return atomic_load_explicit(&g_reactors_count, memory_order_acquire);
}

lm_reactor_t* lm_reactor_by_idx_retained(int idx) {
    if (idx < 0 || idx >= LM_REACTOR_MAX_REG) return NULL;
    pthread_mutex_lock(&g_reactors_mutex);
    lm_reactor_t* r = g_reactors[idx];
    /* 锁内 retain：unregister→release 与本序列在同一把锁上串行，
     * 拿到的指针在 release 前必然有效。 */
    if (r) lm_reactor_retain(r);
    pthread_mutex_unlock(&g_reactors_mutex);
    return r;
}

/* ============================================================
 * T5：入站新连接 MPSC 队列
 * ============================================================ */

/* 节点取池：先复用 freelist（owner pop 后归还），否则 malloc。持 mtx 调用。 */
static lm_inbound_t* inbound_node_take_locked(lm_reactor_t* r) {
    lm_inbound_t* n = r->inbound_free;
    if (n) {
        r->inbound_free = n->next;
        return n;
    }
    return (lm_inbound_t*)malloc(sizeof(lm_inbound_t));
}

int lm_reactor_submit_fd(lm_reactor_t* r, int fd, int kind,
                         const struct sockaddr* addr, socklen_t addrlen,
                         int max_conn) {
    if (!r || fd < 0) return -2;
    lm_co_t* wake_co = NULL;
    lm_scheduler_t* wake_sched = NULL;
    pthread_mutex_lock(&r->inbound_mtx);
    /* 权威容量门控（锁内，与 publish_load 校准线性化）：reserved 已达
     * per-worker 上限即拒，acceptor 换下一个 worker；突发受理也不超卖。 */
    if (max_conn > 0 &&
        atomic_load_explicit(&r->inbound_reserved, memory_order_acquire) >= max_conn) {
        pthread_mutex_unlock(&r->inbound_mtx);
        return -1;
    }
    if (r->inbound_pending >= r->inbound_cap) {
        pthread_mutex_unlock(&r->inbound_mtx);
        return -1;   /* 有界队列满：调用方换 worker / 停 accept 反压 */
    }
    lm_inbound_t* n = inbound_node_take_locked(r);
    if (!n) {
        pthread_mutex_unlock(&r->inbound_mtx);
        return -2;
    }
    n->fd = fd;
    n->kind = kind;
    n->next = NULL;
    n->addrlen = 0;
    memset(&n->addr, 0, sizeof(n->addr));
    if (addr && addrlen > 0) {
        socklen_t cp = addrlen;
        if (cp > (socklen_t)sizeof(n->addr)) cp = (socklen_t)sizeof(n->addr);
        memcpy(&n->addr, addr, cp);
        n->addrlen = cp;
    }
    if (r->inbound_tail) r->inbound_tail->next = n;
    else r->inbound_head = n;
    r->inbound_tail = n;
    r->inbound_pending++;
    /* 占 reservation：入队即计入权威占用，直到 owner publish 校准
     *（active+pending 仍含本 fd，计数不丢）。 */
    atomic_fetch_add_explicit(&r->inbound_reserved, 1, memory_order_acq_rel);
    /* owner receiver 正在等：一次性摘走等待者并在解锁后唤醒（post + self-pipe，
     * 跨线程安全）。未在等说明 receiver 正在 pop/处理循环中，必将自行取到，
     * 无需 wakeup（fd 本身尚未挂任何 epoll，唤醒只服务于等待协程）。 */
    if (r->inbound_wait_co) {
        wake_co = (lm_co_t*)r->inbound_wait_co;
        wake_sched = (lm_scheduler_t*)r->inbound_wait_sched;
        r->inbound_wait_co = NULL;
        r->inbound_wait_sched = NULL;
    }
    pthread_mutex_unlock(&r->inbound_mtx);
    if (wake_co && wake_sched) lm_scheduler_wakeup(wake_sched, wake_co);
    return 0;
}

int lm_reactor_inbound_pop(lm_reactor_t* r, lm_inbound_t* out) {
    if (!r || !out) return 0;
    int got = 0;
    pthread_mutex_lock(&r->inbound_mtx);
    lm_inbound_t* n = r->inbound_head;
    if (n) {
        r->inbound_head = n->next;
        if (!r->inbound_head) r->inbound_tail = NULL;
        r->inbound_pending--;
        *out = *n;                         /* POD 拷贝（含 fd/kind/addr） */
        out->next = NULL;
        n->next = r->inbound_free;         /* 节点回 freelist 复用 */
        r->inbound_free = n;
        got = 1;
    }
    pthread_mutex_unlock(&r->inbound_mtx);
    return got;
}

int lm_reactor_inbound_arm(lm_reactor_t* r, void* co, void* sched) {
    if (!r || !co) return 0;
    int armed = 0;
    pthread_mutex_lock(&r->inbound_mtx);
    /* 锁内复查：提交与登记在同一把锁上串行——
     * 先入队则此处必见非空（调用方立即再 pop，不丢连接）；
     * 先登记则提交方负责摘等待者并唤醒。 */
    if (!r->inbound_head) {
        r->inbound_wait_co = co;
        r->inbound_wait_sched = sched;
        armed = 1;
    }
    pthread_mutex_unlock(&r->inbound_mtx);
    return armed;
}

void lm_reactor_inbound_cancel(lm_reactor_t* r, void* co) {
    if (!r || !co) return;
    pthread_mutex_lock(&r->inbound_mtx);
    if (r->inbound_wait_co == co) {
        r->inbound_wait_co = NULL;
        r->inbound_wait_sched = NULL;
    }
    pthread_mutex_unlock(&r->inbound_mtx);
}

int lm_reactor_inbound_pending(lm_reactor_t* r) {
    if (!r) return 0;
    pthread_mutex_lock(&r->inbound_mtx);
    int n = r->inbound_pending;
    pthread_mutex_unlock(&r->inbound_mtx);
    return n;
}

int lm_reactor_inbound_capacity(lm_reactor_t* r) {
    return r ? r->inbound_cap : 0;
}

void lm_reactor_publish_load(lm_reactor_t* r, int load) {
    if (!r) return;
    /* 入站锁内校准：reserved = 在役 + 已入队未 pop。与 submit 的
     * reserve 自增在同一把锁线性化——publish 覆盖不会丢失提交增量
     *（增量若已入队则体现在 pending，若已被 pop 则体现在 load）。 */
    pthread_mutex_lock(&r->inbound_mtx);
    atomic_store_explicit(&r->inbound_load, load, memory_order_release);
    atomic_store_explicit(&r->inbound_reserved,
                          load + r->inbound_pending, memory_order_release);
    pthread_mutex_unlock(&r->inbound_mtx);
}

int lm_reactor_inbound_load(lm_reactor_t* r) {
    return r ? atomic_load_explicit(&r->inbound_load, memory_order_acquire) : 0;
}

int lm_reactor_inbound_reserved(lm_reactor_t* r) {
    return r ? atomic_load_explicit(&r->inbound_reserved, memory_order_acquire) : 0;
}
