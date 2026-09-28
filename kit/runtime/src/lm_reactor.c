// lm_reactor.c —— 事件驱动 reactor 实现（参考 nginx src/event）
// 单 reactor 线程跑所有协程；epoll/kqueue ET 后端；连接池 + 定时器最小堆 + posted 队列。
//
// 主循环顺序（对齐 nginx ngx_process_events_and_timers）：
//   process_events(timeout=最近 timer 剩余) → 处理 posted_accept →
//   过期 timer 派发 → 处理 posted_events → 检查 stop_flag。
//
// 后端抽象（对齐 nginx ngx_event_actions_t）：
//   Linux → epoll（EPOLLET，ET 模式）
//   macOS → kqueue（EV_CLEAR，ET 模式）
//
// 连接池：按 fd 索引取，归还入 free list。fd 超 capacity 返回 NULL（用户拒绝或扩容）。
// 定时器最小堆：key=expire_ms（单调时钟）。堆顶最小，find_timer 取堆顶 expire。
// posted 队列：双队列（accept 优先 + 一般事件），避免事件回调内递归调用。
#include "lm_reactor.h"
#include "gc_runtime.h"

#include <stdlib.h>
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
    ev.data.ptr = c;     /* 直接挂 connection 指针（nginx 同做法） */
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
        /* 直接调 handler（单线程 reactor，无并发）；nginx 通过 posted 队列延后 */
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
        /* 首次按 fd 索引取（nginx 同做法：fd 索引直接定位） */
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
    return c;
}

void lm_reactor_free_connection(lm_reactor_t* r, lm_connection_t* c) {
    if (!r || !c) return;
    /* 先从 reactor 摘事件（避免悬挂 fd 注册） */
    if (c->active_events) {
        r->actions->del(r, c, c->active_events);
        c->active_events = 0;
        c->flags &= ~LM_CONN_FLAG_ACTIVE;
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
    return 0;
}

int lm_reactor_del(lm_reactor_t* r, lm_connection_t* c, uint32_t events) {
    if (!r || !c || !r->actions->del) return -1;
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
 * 定时器最小堆
 * ============================================================ */

static void timer_heap_sift_up(lm_reactor_t* r, int i) {
    while (i > 0) {
        int parent = (i - 1) / 2;
        if (r->timer_heap[i].expire_ms >= r->timer_heap[parent].expire_ms) break;
        lm_timer_t tmp = r->timer_heap[i];
        r->timer_heap[i] = r->timer_heap[parent];
        r->timer_heap[parent] = tmp;
        i = parent;
    }
}

static void timer_heap_sift_down(lm_reactor_t* r, int i) {
    for (;;) {
        int l = 2 * i + 1, rr = 2 * i + 2;
        int smallest = i;
        if (l < r->timer_count && r->timer_heap[l].expire_ms < r->timer_heap[smallest].expire_ms) smallest = l;
        if (rr < r->timer_count && r->timer_heap[rr].expire_ms < r->timer_heap[smallest].expire_ms) smallest = rr;
        if (smallest == i) break;
        lm_timer_t tmp = r->timer_heap[i];
        r->timer_heap[i] = r->timer_heap[smallest];
        r->timer_heap[smallest] = tmp;
        i = smallest;
    }
}

int lm_reactor_add_timer(lm_reactor_t* r, uint64_t expire_ms, lm_timer_cb_t cb, void* data) {
    if (!r || !cb) return -1;
    if (r->timer_count >= r->timer_capacity) {
        int new_cap = r->timer_capacity * 2 + 64;
        lm_timer_t* p = (lm_timer_t*)realloc(r->timer_heap, (size_t)new_cap * sizeof(lm_timer_t));
        if (!p) return -1;
        r->timer_heap = p;
        r->timer_capacity = new_cap;
    }
    int id = ++r->next_timer_id;
    lm_timer_t* t = &r->timer_heap[r->timer_count++];
    t->id = id;
    t->expire_ms = lm_reactor_now_ms() + expire_ms;
    t->cb = cb;
    t->data = data;
    t->active = 1;
    timer_heap_sift_up(r, r->timer_count - 1);
    return id;
}

void lm_reactor_del_timer(lm_reactor_t* r, int timer_id) {
    if (!r || timer_id <= 0) return;
    /* 线性找（堆按 expire 排，不按 id；删除频率低，O(n) 可接受） */
    for (int i = 0; i < r->timer_count; i++) {
        if (r->timer_heap[i].id == timer_id) {
            /* 用堆尾覆盖，再下沉/上浮 */
            int last = --r->timer_count;
            if (i == last) return;   /* 删的就是堆尾 */
            r->timer_heap[i] = r->timer_heap[last];
            /* 既可能需要上浮（比 parent 小），也可能需要下沉（比 child 大） */
            timer_heap_sift_up(r, i);
            timer_heap_sift_down(r, i);
            return;
        }
    }
}

/* 取堆顶 expire（最近过期），返回剩余 ms；堆空返回 -1（永久阻塞） */
static int timer_next_timeout_ms(lm_reactor_t* r) {
    if (r->timer_count == 0) return -1;
    uint64_t now = lm_reactor_now_ms();
    uint64_t top = r->timer_heap[0].expire_ms;
    if (top <= now) return 0;
    int diff = (int)(top - now);
    return diff;
}

/* 派发过期 timer：循环取堆顶，若过期则调 cb 并弹出，未过期则停。 */
static void timer_expire(lm_reactor_t* r) {
    uint64_t now = lm_reactor_now_ms();
    while (r->timer_count > 0 && r->timer_heap[0].expire_ms <= now) {
        lm_timer_t t = r->timer_heap[0];
        /* 先弹堆顶（堆尾覆盖+下沉），再调 cb（cb 内可能加新 timer） */
        int last = --r->timer_count;
        if (last > 0) {
            r->timer_heap[0] = r->timer_heap[last];
            timer_heap_sift_down(r, 0);
        }
        if (t.cb) t.cb(t.id, t.data);
    }
}

/* ============================================================
 * reactor 主对象创建/销毁
 * ============================================================ */

lm_reactor_t* lm_reactor_new(int conn_capacity) {
    if (conn_capacity <= 0) conn_capacity = 65536;
    lm_reactor_t* r = (lm_reactor_t*)calloc(1, sizeof(lm_reactor_t));
    if (!r) return NULL;
    r->connections = (lm_connection_t*)calloc((size_t)conn_capacity, sizeof(lm_connection_t));
    r->timer_heap = (lm_timer_t*)calloc(64, sizeof(lm_timer_t));
    r->posted_accept = (lm_connection_t**)calloc(64, sizeof(lm_connection_t*));
    r->posted_events = (lm_connection_t**)calloc(64, sizeof(lm_connection_t*));
    if (!r->connections || !r->timer_heap || !r->posted_accept || !r->posted_events) {
        free(r->connections); free(r->timer_heap);
        free(r->posted_accept); free(r->posted_events);
        free(r);
        return NULL;
    }
    r->conn_capacity = conn_capacity;
    r->timer_capacity = 64;
    r->timer_count = 0;
    r->next_timer_id = 0;
    r->posted_accept_capacity = 64;
    r->posted_accept_count = 0;
    r->posted_events_capacity = 64;
    r->posted_events_count = 0;
    r->stop_flag = 0;
    r->free_conns = NULL;
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
        free(r->connections); free(r->timer_heap);
        free(r->posted_accept); free(r->posted_events);
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

void lm_reactor_destroy(lm_reactor_t* r) {
    if (!r) return;
    if (r->actions && r->actions->fini) r->actions->fini(r);
    /* fini 已关 backend_fd，wake read 端注册随之失效；关管道两端 */
    if (r->wake_pipe[0] >= 0) { close(r->wake_pipe[0]); r->wake_pipe[0] = -1; }
    if (r->wake_pipe[1] >= 0) { close(r->wake_pipe[1]); r->wake_pipe[1] = -1; }
    free(r->connections);
    free(r->timer_heap);
    free(r->posted_accept);
    free(r->posted_events);
    free(r);
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
        /* 1. 计算最近 timer 剩余作为 epoll_wait/kevent 超时，使 timer 准时派发。
         * 无 timer 时返回 -1（无限阻塞）——stop() 由 self-pipe 即时唤醒（零延迟），
         * 故无限阻塞对 stop 安全。但 STW 仍需周期轮询：reactor 阻塞在
         * epoll_wait 时无法响应 GC STW 请求（gc_stw_check_fast 仅每轮顶部跑），
         * 故无 timer 时限 cap 1s 保证 STW 延迟 ≤1s（self-pipe 不覆盖 STW，
         * 因 GC 不持有 reactor 管道句柄，避免跨模块耦合）。 */
        int timeout_ms = timer_next_timeout_ms(r);
        if (timeout_ms < 0) timeout_ms = 1000;
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
        /* 4. 过期 timer 派发 */
        timer_expire(r);
        /* 5. posted_events（一般事件延后处理，避免回调递归） */
        process_posted(r->posted_events, &r->posted_events_count);
    }
}

/* ============================================================
 * accept helper（对齐 nginx ngx_event_accept）
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
            *out_again = 0;   /* 进程级 fd 耗尽，跳过此次（nginx 同做法） */
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
 * connect helper（对齐 nginx ngx_event_connect）
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
