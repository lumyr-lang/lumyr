// lm_socket.c —— 网络套接字对象（VAL_SOCKET）
// 统一封装 TCP / UDP / Unix 域流 / Unix 域数据报 套接字
// 持有 fd 描述符；close() 关闭并置 closed=1，GC 回收时兜底关闭
// 错误码：全部用 runtime_error_code(NET_ERR_*, "SocketError", msg) 抛出，
// 与 lm 层 LumyrNetWork/NetError.lm 的 enum NetError 数值逐项对齐，
// 上层 catch(e) 后 e.getCode() == NetError.XXX 精确分派（不再字符串匹配）。
#include "lm_socket.h"
#include "lm_net_errno.h"
#include "lm_array.h"
#include "lm_string.h"
#include "lm_container.h"
#include "gc_runtime.h"
#include "lm_reactor.h"
#include "lm_co.h"
#include "lm_scheduler.h"   /* Phase 8.4：timeout/resume 走 scheduler 队列去重 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#ifndef _WIN32
#include <signal.h>
#endif

/* 进程级忽略 SIGPIPE：向已关闭连接 send 时返回 EPIPE 错误而非终止进程。
 * constructor 属性使 VM 通道（lumyr 自身）与 C 生成通道（链接 libruntime
 * 的产物）在进程启动时自动生效，无需业务层干预。 */
#ifndef _WIN32
__attribute__((constructor))
static void lm_socket_ignore_sigpipe(void) {
    signal(SIGPIPE, SIG_IGN);
}
#endif

/* ============================================================
 * 内部辅助
 * ============================================================ */

// 根据类型返回字符串名
static const char* kind_name(SocketKind k) {
    switch(k) {
    case SOCK_KIND_TCP:          return "tcp";
    case SOCK_KIND_UDP:          return "udp";
    case SOCK_KIND_UNIX_STREAM: return "unix";
    case SOCK_KIND_UNIX_DGRAM:  return "unix_dgram";
    default: return "unknown";
    }
}

// 是否 Unix 域类型
static int kind_is_unix(SocketKind k) {
    return k == SOCK_KIND_UNIX_STREAM || k == SOCK_KIND_UNIX_DGRAM;
}

// 创建底层 socket fd
static int create_fd(SocketKind k) {
    int fd = -1;
    if(k == SOCK_KIND_TCP) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
    } else if(k == SOCK_KIND_UDP) {
        fd = socket(AF_INET, SOCK_DGRAM, 0);
    } else if(k == SOCK_KIND_UNIX_STREAM) {
        fd = socket(AF_UNIX, SOCK_STREAM, 0);
    } else if(k == SOCK_KIND_UNIX_DGRAM) {
        fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    }
    return fd;
}

// 报错包装：拼 errno 文本并 runtime_error_code（带操作对应的枚举码）
static void sock_error_code(const char* op, NetErrorCode code) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s() 失败: %s", op, strerror(errno));
    runtime_error_code(code, "SocketError", buf);
}

/* DNS 解析超时（毫秒，0 = 不限，默认）：build_inet 内若 > 0 走 worker thread +
 * cond_timedwait 路径。worker 超时后 detach 继续跑（结果丢弃），避免 SIGALRM
 * 中断 getaddrinfo 导致 libc 内部锁死。 */
static int g_dns_timeout_ms = 0;

void lm_socket_set_dns_timeout_ms(int ms) {
    g_dns_timeout_ms = ms > 0 ? ms : 0;
}

/* ============================================================
 * 协程模式（reactor + 协程）：socket API 双模式
 * ============================================================
 * 协程上下文（lm_co_current() != NULL）且已 lm_socket_set_reactor 时，
 * recv/send/accept/connect 自动走非阻塞 + yield 等事件路径：
 *   1. 设 fd 非阻塞（否则单次 syscall 阻塞整个 reactor）
 *   2. syscall 返回 EAGAIN/EINPROGRESS → co_wait_fd 注册事件 + lm_co_yield
 *   3. reactor 主循环 epoll_wait/kevent 等事件就绪
 *   4. 事件回调 co_resume_handler → lm_co_resume(co)
 *   5. 协程从 yield 点后继续，co_wait_fd 摘事件 + free_connection 返回
 *   6. syscall 重试（循环补齐直到完成或真错）
 *
 * 不调 gc_enter_native_block 包裹 yield：swapcontext 返回后 reactor 主循环
 * 继续运行并可能变更 GC 根（at_safepoint=1 会误报安全点 → 并发 GC 扫描期间
 * 根被修改 → UAF）。STW 轮询由 reactor 主循环每轮 gc_stw_check_fast() 承担。
 * 协程栈扫描由 gc_register_coroutine 在 lm_co_yield 内注册（Phase 3 已做）。
 *
 * 非协程上下文：保持原有阻塞逻辑（gc_enter_native_block + 阻塞 syscall），
 * 兼容 thread-per-conn HTTP client 路径。 */

/* Phase 7.1：g_socket_reactor 改 _Thread_local，每线程独立持有自己的 reactor。
 * 行为兼容：单线程下 thread_local 等效全局变量，ServiceApplication 既有行为不变；
 * 多线程下各线程可独立 setReactor 互不覆盖，为 per-thread scheduler + 多 reactor
 * 并存打底。lm_socket_set_reactor 只写当前线程的 TLS，co_wait_fd 只读当前线程的 TLS。 */
static _Thread_local lm_reactor_t* g_socket_reactor = NULL;

void lm_socket_set_reactor(lm_reactor_t* r) {
    g_socket_reactor = r;
}

/* 设 fd 非阻塞（协程模式必须，否则 recv/send 阻塞整个 reactor）。
 * 成功返回 0，失败 -1。 */
static int set_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

/* 协程事件回调：fd 事件就绪时把等待的协程投递到 scheduler 就绪队列。
 * Phase 8.4：不再直接 lm_co_resume——超时回调在 timer 线程跨线程并发，
 * 两者直接 resume 同一 co 会双线程同栈腐败。统一走 scheduler 队列 +
 * co->queued 去重：fd 事件（本回调，reactor 线程=owner）走 post_local
 * 快通道，超时（co_timeout_handler，timer 线程）走 wakeup 跨线程 post；
 * 先到者 queued=1，后到者 no-op，由 reactor 主循环 step1.5 drain 统一 resume。
 * 无 scheduler（conn->sched==NULL，Phase 5 简化遗留）：直接 resume（同线程
 * 串行安全，超时路径已降级不并发）。 */
static void co_resume_handler(lm_connection_t* conn, uint32_t events, void* data) {
    (void)events;
    lm_co_t* co = (lm_co_t*)data;
    if (!co) return;
    if (conn->sched) {
        lm_scheduler_post_local(conn->sched, co);
    } else {
        lm_co_resume(co);
    }
}

/* 协程超时回调（Phase 8.4：全局 TimerThread 到期，timer 线程上下文）：
 * 置 conn->timedout=1 并把协程投递回注册归属 scheduler。
 * 跨线程（timer→reactor）：走 lm_scheduler_wakeup（post + self-pipe 唤醒目标
 * reactor），由目标 reactor 主循环 step1.5 drain 统一 resume。queued 去重保证
 * 与 fd 事件的 post_local 不会双 resume 同一 co。
 * 无 scheduler（conn->sched==NULL）：仅置 timedout 不 wake——co 留 SUSPENDED，
 * 降级（文档化：无 scheduler 不支持超时精确唤醒，不腐败栈）。 */
static void co_timeout_handler(lm_timer_id_t timer_id, void* data) {
    (void)timer_id;
    lm_connection_t* conn = (lm_connection_t*)data;
    conn->timedout = 1;
    if (!conn->sched) return;
    lm_co_t* co = (lm_co_t*)conn->co;
    if (co) lm_scheduler_wakeup(conn->sched, co);
}

/* 协程挂起等 fd 事件（带超时变体）：fd 事件 + reactor 一次性定时器双注册，先到先得。
 * timeout_ms <= 0 时不注册定时器，退化为无超时等待。
 * 返回 0 事件就绪 / 1 超时 / -1 失败（reactor 未设、连接池满或定时器注册失败）。
 * 必须在协程内调用（lm_co_current() != NULL）。
 * 流程：get_connection（复用时 timedout 已被清零）→ 设 handler/data → add 事件
 * → 注册定时器 → yield → resume 后摘事件 + 摘定时器（已触发时 del 为 no-op，
 * timer_id 单调不复用故无歧义）→ 归还连接池 → 按 timedout 区分返回。 */
static int co_wait_fd_timeout(int fd, int want_read, int want_write, int timeout_ms) {
    lm_co_t* co = lm_co_current();
    if (!co || !g_socket_reactor) return -1;
    /* Phase 8.2：fd 等待自动置 pinned——本协程的 IO 事件挂在当前线程 reactor 上，
     * 唤醒必须回到本线程的 scheduler（scheduler post 分流依据，见 lm_co.h）。
     * 置位后该协程永不入 WSQ / 不被窃取，跨线程 wakeup 走 mutex 定向队列。 */
    co->pinned = 1;
    lm_connection_t* conn = lm_reactor_get_connection(g_socket_reactor, fd);
    if (!conn) return -1;
    uint32_t events = 0;
    if (want_read)  events |= LM_EVENT_READ;
    if (want_write) events |= LM_EVENT_WRITE;
    conn->read_handler  = want_read  ? co_resume_handler : NULL;
    conn->write_handler = want_write ? co_resume_handler : NULL;
    conn->read_data  = co;
    conn->write_data = co;
    conn->co = co;
    /* Phase 8.4：记录所属 scheduler，超时回调（timer 线程跨线程）据此 wakeup
     * 投递协程回本线程 reactor；fd 事件 handler（reactor 线程=本线程）据此
     * post_local。无 scheduler（lm_scheduler_get_current()==NULL）时 conn->sched
     * 保持 NULL，两条路径降级（resume 直接 resume / timeout 仅置标记）。 */
    conn->sched = lm_scheduler_get_current();
    if (lm_reactor_add(g_socket_reactor, conn, events) != 0) {
        lm_reactor_free_connection(g_socket_reactor, conn);
        return -1;
    }
    lm_timer_id_t timer_id = LM_TIMER_INVALID_ID;
    if (timeout_ms > 0) {
        timer_id = lm_reactor_add_timer(g_socket_reactor, (uint64_t)timeout_ms,
                                        co_timeout_handler, conn);
        if (timer_id == LM_TIMER_INVALID_ID) {
            lm_reactor_del(g_socket_reactor, conn, conn->active_events);
            lm_reactor_free_connection(g_socket_reactor, conn);
            return -1;
        }
    }
    lm_co_yield();  /* 切回 reactor 主循环等事件/超时 */
    /* resume 回来：事件已就绪或定时器到期，摘事件 + 摘定时器 + 归还连接池 */
    int timedout = conn->timedout;
    lm_reactor_del(g_socket_reactor, conn, conn->active_events);
    if (timer_id != LM_TIMER_INVALID_ID) lm_reactor_del_timer(g_socket_reactor, timer_id);
    lm_reactor_free_connection(g_socket_reactor, conn);
    return timedout ? 1 : 0;
}

/* 协程挂起等 fd 事件：注册 want_read/want_write 事件 → yield → resume 后摘事件。
 * 返回 0 成功（事件就绪，可重试 syscall）；-1 失败（reactor 未设或连接池满）。
 * 必须在协程内调用（lm_co_current() != NULL）。
 * 流程：get_connection → 设 handler/data → add 事件 → yield → resume → del + free。 */
static int co_wait_fd(int fd, int want_read, int want_write) {
    return co_wait_fd_timeout(fd, want_read, want_write, 0) == 0 ? 0 : -1;
}

/* 协程模式非阻塞 connect 共用逻辑（unix/inet 都调这）。
 * 返回 0 成功（立即连上或 EINPROGRESS 后等可写事件 resume getsockopt=0）；
 * -1 失败（errno 已设为合理值，caller 调 sock_error_code 拼错误消息）。 */
static int co_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    set_nonblock(fd);
    int rc = connect(fd, addr, addrlen);
    if (rc == 0) return 0;  /* 立即连上（本地回环） */
    if (errno != EINPROGRESS) return -1;  /* 真错 */
    /* EINPROGRESS：挂写事件等可写 */
    if (co_wait_fd(fd, 0, 1) != 0) { errno = EIO; return -1; }
    int err = 0; socklen_t errlen = sizeof(err);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) != 0) return -1;
    if (err != 0) { errno = err; return -1; }
    return 0;
}

typedef struct {
    const char* host;
    struct sockaddr_in* addr;
    int rc;            /* 0 成功, -1 失败 */
    pthread_mutex_t mtx;
    pthread_cond_t cv;
    int done;
} DnsResolveCtx;

static void* dns_worker(void* arg) {
    DnsResolveCtx* ctx = (DnsResolveCtx*)arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    struct sockaddr_in resolved;
    int rc = -1;
    if(getaddrinfo(ctx->host, NULL, &hints, &res) == 0 && res) {
        memcpy(&resolved, res->ai_addr, sizeof(struct sockaddr_in));
        rc = 0;
        freeaddrinfo(res);
    }
    /* 在 mutex 内写 addr 与 done：主线程拿到 mutex 后读，无数据竞争 */
    pthread_mutex_lock(&ctx->mtx);
    if (rc == 0) {
        memcpy(ctx->addr, &resolved, sizeof(struct sockaddr_in));
    }
    ctx->rc = rc;
    ctx->done = 1;
    pthread_cond_signal(&ctx->cv);
    pthread_mutex_unlock(&ctx->mtx);
    return NULL;
}

/* 同步解析（无超时），供 build_inet 超时分支回退与未启用超时路径共用 */
static int build_inet_sync(const char* host, struct sockaddr_in* addr) {
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    if(getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        if(res) freeaddrinfo(res);
        return -1;
    }
    memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    freeaddrinfo(res);
    return 0;
}

// 解析 host+port → sockaddr_in（IPv4），返回 0 成功，-1 解析失败，-2 超时
static int build_inet(const char* host, int port, struct sockaddr_in* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);
    if(!host || strcmp(host, "0.0.0.0") == 0 || strcmp(host, "*") == 0) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }
    // 先尝试数字地址（快路径）
    if(inet_pton(AF_INET, host, &addr->sin_addr) == 1) return 0;
    int timeout = g_dns_timeout_ms;
    if (timeout <= 0) {
        return build_inet_sync(host, addr);
    }
    // 超时路径：worker thread 跑 getaddrinfo，主线程等 cond_timedwait
    DnsResolveCtx ctx;
    ctx.host = host;
    ctx.addr = addr;
    ctx.rc = -1;
    ctx.done = 0;
    pthread_mutex_init(&ctx.mtx, NULL);
    pthread_cond_init(&ctx.cv, NULL);
    pthread_t tid;
    if (pthread_create(&tid, NULL, dns_worker, &ctx) != 0) {
        /* 创建失败回退同步（不超时），保证解析仍可完成 */
        pthread_mutex_destroy(&ctx.mtx);
        pthread_cond_destroy(&ctx.cv);
        return build_inet_sync(host, addr);
    }
    pthread_mutex_lock(&ctx.mtx);
    while (!ctx.done) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout / 1000;
        long nsec = (long)(timeout % 1000) * 1000000L;
        ts.tv_nsec += nsec;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        int wr = pthread_cond_timedwait(&ctx.cv, &ctx.mtx, &ts);
        if (wr == ETIMEDOUT) break;
    }
    int rc = ctx.rc;
    int done = ctx.done;
    pthread_mutex_unlock(&ctx.mtx);
    pthread_cond_destroy(&ctx.cv);
    pthread_mutex_destroy(&ctx.mtx);
    if (!done) {
        /* 超时：worker 继续跑（结果丢弃），detach 避免 pthread_t 句柄泄漏 */
        pthread_detach(tid);
        return -2;
    }
    pthread_join(tid, NULL);
    if (rc == 0) {
        addr->sin_port = htons((uint16_t)port);
    }
    return rc;
}

// 解析 unix path → sockaddr_un
static void build_unix(const char* path, struct sockaddr_un* addr) {
    memset(addr, 0, sizeof(*addr));
    addr->sun_family = AF_UNIX;
    strncpy(addr->sun_path, path ? path : "", sizeof(addr->sun_path) - 1);
}

/* ============================================================
 * 公共 API
 * ============================================================ */

Value lumyr_socket_make(int kind) {
    SocketObj* o = (SocketObj*)gc_alloc(sizeof(SocketObj), VAL_SOCKET);
    if(!o) { Value z; z.type = VAL_NONE; z.str_inline = 0; return z; }
    o->fd = create_fd((SocketKind)kind);
    o->kind = (SocketKind)kind;
    o->is_server = 0;
    o->is_connected = 0;
    o->closed = 0;
    o->stack_alloc = 0;
    o->recv_timeout_ms = 0;
    o->send_timeout_ms = 0;
    if(o->fd < 0) {
        /* 创建失败不中断（fileno() 返回 -1，后续操作报错） */
        o->fd = -1;
    }
    Value r;
    r.type = VAL_SOCKET;
    r.str_inline = 0;
    r.v.socket_obj = o;
    return r;
}

Value lumyr_socket_from_fd(int fd, int kind, int connected) {
    SocketObj* o = (SocketObj*)gc_alloc(sizeof(SocketObj), VAL_SOCKET);
    if(!o) { Value z; z.type = VAL_NONE; z.str_inline = 0; return z; }
    o->fd = fd;
    o->kind = (SocketKind)kind;
    o->is_server = 0;
    o->is_connected = (uint8_t)(connected ? 1 : 0);
    o->closed = 0;
    o->stack_alloc = 0;
    o->recv_timeout_ms = 0;
    o->send_timeout_ms = 0;
    Value r;
    r.type = VAL_SOCKET;
    r.str_inline = 0;
    r.v.socket_obj = o;
    return r;
}

Value lumyr_socket_field(Value v, const char* name) {
    if(!name) return lumyr_make_int(0);
    if(v.type != VAL_SOCKET) return lumyr_make_int(0);
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return lumyr_make_int(0);
    if(strcmp(name, "fd") == 0)        return lumyr_make_int(o->fd);
    if(strcmp(name, "kind") == 0)     return lumyr_make_string(kind_name(o->kind));
    if(strcmp(name, "closed") == 0)   return lumyr_make_bool(o->closed ? 1 : 0);
    if(strcmp(name, "connected") == 0) return lumyr_make_bool(o->is_connected ? 1 : 0);
    if(strcmp(name, "isServer") == 0) return lumyr_make_bool(o->is_server ? 1 : 0);
    if(strcmp(name, "isConnected") == 0) return lumyr_make_bool(o->is_connected ? 1 : 0);
    /* 无兜底：未知字段/方法名 → AttributeError（方法解析已先完成），
     * 禁止静默返回 0 */
    {
        char buf[256];
        snprintf(buf, sizeof buf,
                 "socket 没有字段或方法 \"%s\" / socket has no field or method \"%s\"",
                 name, name);
        runtime_error_code(NET_ERR_NO_FIELD, "SocketError", buf);
    }
    return lumyr_make_int(0);   /* 不可达 */
}

char* lumyr_socket_to_str(Value v) {
    if(v.type != VAL_SOCKET) return strdup("");
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return strdup("");
    char buf[128];
    snprintf(buf, sizeof(buf), "<socket %s fd=%d>", kind_name(o->kind), o->closed ? -1 : o->fd);
    return strdup(buf);
}

Value lumyr_socket_connect(Value v, const char* host, int port) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "connect() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "connect() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error_code(NET_ERR_BAD_FD, "SocketError", "connect() 套接字 fd 无效"); return val_none(); }
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        if (in_co) {
            /* 协程模式：非阻塞 connect，EINPROGRESS 挂写事件等可写 */
            if (co_connect(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                sock_error_code("connect", NET_ERR_CONNECT);
                return val_none();
            }
        } else {
            /* 阻塞连接期间标记 GC 安全点（同 sleep），错误处理在 leave 之后 */
            gc_enter_native_block();
            int rc = connect(o->fd, (struct sockaddr*)&addr, sizeof(addr));
            gc_leave_native_block();
            if(rc != 0) {
                sock_error_code("connect", NET_ERR_CONNECT);
                return val_none();
            }
        }
    } else {
        struct sockaddr_in addr;
        if(build_inet(host ? host : "127.0.0.1", port, &addr) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "connect() 无法解析地址: %s", host ? host : "");
            runtime_error_code(NET_ERR_ADDR_RESOLVE, "SocketError", buf);
            return val_none();
        }
        if (in_co) {
            if (co_connect(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
                sock_error_code("connect", NET_ERR_CONNECT);
                return val_none();
            }
        } else {
            gc_enter_native_block();
            int rc = connect(o->fd, (struct sockaddr*)&addr, sizeof(addr));
            gc_leave_native_block();
            if(rc != 0) {
                sock_error_code("connect", NET_ERR_CONNECT);
                return val_none();
            }
        }
    }
    o->is_connected = 1;
    return val_none();
}

Value lumyr_socket_bind(Value v, const char* host, int port) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "bind() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "bind() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error_code(NET_ERR_BAD_FD, "SocketError", "bind() 套接字 fd 无效"); return val_none(); }
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        /* Unix 域路径已存在则先清理，避免 EADDRINUSE */
        unlink(addr.sun_path);
        if(bind(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error_code("bind", NET_ERR_BIND);
            return val_none();
        }
    } else {
        struct sockaddr_in addr;
        if(build_inet(host ? host : "0.0.0.0", port, &addr) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "bind() 无法解析地址: %s", host ? host : "");
            runtime_error_code(NET_ERR_ADDR_RESOLVE, "SocketError", buf);
            return val_none();
        }
        if(bind(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error_code("bind", NET_ERR_BIND);
            return val_none();
        }
    }
    /* bind 即服务端绑定（TCP 随后 listen 会再置位；UDP/Unix 数据报 bind 即可接收） */
    o->is_server = 1;
    return val_none();
}

Value lumyr_socket_listen(Value v, int backlog) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "listen() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "listen() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error_code(NET_ERR_BAD_FD, "SocketError", "listen() 套接字 fd 无效"); return val_none(); }
    if(backlog <= 0) backlog = 128;
    if(listen(o->fd, backlog) != 0) {
        sock_error_code("listen", NET_ERR_LISTEN);
        return val_none();
    }
    o->is_server = 1;
    return val_none();
}

Value lumyr_socket_accept(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "accept() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "accept() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error_code(NET_ERR_BAD_FD, "SocketError", "accept() 套接字 fd 无效"); return val_none(); }
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if (in_co) {
        set_nonblock(o->fd);  /* listen_fd 非阻塞，否则 accept 阻塞整个 reactor */
    }
    struct sockaddr_storage cli;
    socklen_t clilen = sizeof(cli);
    int cfd;
    for (;;) {
        if (in_co) {
            cfd = accept(o->fd, (struct sockaddr*)&cli, &clilen);
            if (cfd < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* ET 模式无更多连接，挂读事件等可读，resume 后重试 */
                if (co_wait_fd(o->fd, 1, 0) != 0) {
                    sock_error_code("accept", NET_ERR_ACCEPT);
                    return val_none();
                }
                continue;
            }
            if (cfd >= 0) {
                /* 新 cfd 设非阻塞 + CLOEXEC（协程模式必须非阻塞，否则下次 recv/send 阻塞） */
                int fl = fcntl(cfd, F_GETFL, 0);
                if (fl != -1) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
                int fd = fcntl(cfd, F_GETFD, 0);
                if (fd != -1) fcntl(cfd, F_SETFD, fd | FD_CLOEXEC);
            }
        } else {
            /* 阻塞 accept 期间标记 GC 安全点（同 sleep），错误处理在 leave 之后 */
            gc_enter_native_block();
            cfd = accept(o->fd, (struct sockaddr*)&cli, &clilen);
            gc_leave_native_block();
        }
        break;  /* 拿到 cfd 或真错都退出 */
    }
    if(cfd < 0) {
        sock_error_code("accept", NET_ERR_ACCEPT);
        return val_none();
    }
    /* 新客户端套接字，类型与服务端一致，已连接 */
    return lumyr_socket_from_fd(cfd, (int)o->kind, 1);
}

Value lumyr_socket_send(Value v, const char* data, int len, int flags) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "send() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "send() 套接字无效或已关闭"); return lumyr_make_int(-1); }
    if(!data) data = "";
    if(len < 0) len = (int)strlen(data);
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if (in_co) {
        set_nonblock(o->fd);
    }
    /* 循环补齐：send 可能因内核发送缓冲区满而只写部分（n < len），
     * 不补齐会丢数据。循环直到写完或真错。
     * 阻塞模式：SO_SNDTIMEO 到期返回 EAGAIN，按超时码抛错（HTTP 层 catch 转 408）。
     * 协程模式：EAGAIN 是"缓冲区满"正常态，yield 等可写事件 resume 后重试。
     * 已写部分 > 0 时返回已写字节数让上层判断是否续写。 */
    size_t total = 0;
    ssize_t n;
    while (total < (size_t)len) {
        if (in_co) {
            n = send(o->fd, data + total, (size_t)(len - total), flags);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 协程模式：挂写事件等可写，resume 后重试。
                 * per-socket sendTimeout（SocketObj 字段）走 reactor 定时器：
                 * 超时返回 1，已写部分返回让上层判断，完全没写按超时码抛错
                 * （NET_ERR_SEND_TIMEOUT，与阻塞模式 SO_SNDTIMEO 同码）。 */
                int wr = co_wait_fd_timeout(o->fd, 0, 1, o->send_timeout_ms);
                if (wr != 0) {
                    if (total > 0) return lumyr_make_int((int)total);
                    if (wr == 1) {
                        runtime_error_code(NET_ERR_SEND_TIMEOUT, "SocketError", "发送超时 / socket send timeout");
                    } else {
                        sock_error_code("send", NET_ERR_SEND);
                    }
                    return lumyr_make_int(-1);
                }
                continue;
            }
        } else {
            gc_enter_native_block();
            n = send(o->fd, data + total, (size_t)(len - total), flags);
            gc_leave_native_block();
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 阻塞模式 SO_SNDTIMEO 到期：已写部分返回，完全没写按超时码抛错 */
                if (total > 0) return lumyr_make_int((int)total);
                runtime_error_code(NET_ERR_SEND_TIMEOUT, "SocketError", "发送超时 / socket send timeout");
                return lumyr_make_int(-1);
            }
        }
        if (n < 0) {
            /* 真错：已写部分返回让上层判断，完全没写抛错 */
            if (total > 0) return lumyr_make_int((int)total);
            sock_error_code("send", NET_ERR_SEND);
            return lumyr_make_int(-1);
        }
        if (n == 0) break;  /* 对端关闭保险，正常不应发生 */
        total += (size_t)n;
    }
    return lumyr_make_int((int)total);
}

Value lumyr_socket_recv(Value v, int maxLen, int flags, int asBytes) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "recv() 仅适用于 socket 对象"); return lumyr_make_string(""); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "recv() 套接字无效或已关闭"); return lumyr_make_string(""); }
    if(maxLen <= 0) maxLen = 4096;
    char* buf = (char*)malloc((size_t)maxLen + 1);
    if(!buf) { runtime_error_code(NET_ERR_NO_MEMORY, "SocketError", "recv() 内存不足"); return lumyr_make_string(""); }
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if (in_co) {
        set_nonblock(o->fd);
    }
    ssize_t n;
    for (;;) {
        if (in_co) {
            n = recv(o->fd, buf, (size_t)maxLen, flags);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 协程模式：挂读事件等可读，resume 后重试。
                 * per-socket recvTimeout（SocketObj 字段）走 reactor 定时器：
                 * 超时返回 1，按 NET_ERR_RECV_TIMEOUT 抛错（与阻塞模式
                 * SO_RCVTIMEO 同码，上层 HTTP 408 分派自动生效）。 */
                int wr = co_wait_fd_timeout(o->fd, 1, 0, o->recv_timeout_ms);
                if (wr != 0) {
                    free(buf);
                    if (wr == 1) {
                        runtime_error_code(NET_ERR_RECV_TIMEOUT, "SocketError", "接收超时 / socket receive timeout");
                    } else {
                        sock_error_code("recv", NET_ERR_RECV);
                    }
                    return lumyr_make_string("");
                }
                continue;
            }
        } else {
            /* 阻塞接收期间标记 GC 安全点：否则 STW 扫描无法覆盖阻塞在内核的线程，
             * 与超时错误（runtime_error longjmp）交互会导致线程栈对象被误回收。
             * 错误处理（含 longjmp）统一放在 leave 之后，保证恢复 at_safepoint。 */
            gc_enter_native_block();
            n = recv(o->fd, buf, (size_t)maxLen, flags);
            gc_leave_native_block();
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 阻塞模式 SO_RCVTIMEO 到期：专用错误码，上层（如 HTTP 请求读取）可
                 * 区分"客户端长时间不发完整报文"与一般 IO 错误，转 408 */
                free(buf);
                runtime_error_code(NET_ERR_RECV_TIMEOUT, "SocketError", "接收超时 / socket receive timeout");
                return lumyr_make_string("");
            }
        }
        break;  /* 拿到数据或真错或 EOF 都退出 */
    }
    if(n < 0) {
        free(buf);
        sock_error_code("recv", NET_ERR_RECV);
        return lumyr_make_string("");
    }
    if(n == 0) {
        /* 对端关闭 / EOF */
        free(buf);
        o->is_connected = 0;
        return asBytes ? lumyr_bytes_from_buf(NULL, 0) : lumyr_make_string("");
    }
    Value r;
    if(asBytes) {
        /* bytes 路径：二进制安全，保留 NUL */
        r = lumyr_bytes_from_buf((const uint8_t*)buf, (int)n);
    } else {
        buf[n] = '\0';
        r = lumyr_make_string(buf);
    }
    free(buf);
    return r;
}

Value lumyr_socket_sendto(Value v, const char* data, int len,
                          const char* host, int port, int flags) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "sendTo() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "sendTo() 套接字无效或已关闭"); return lumyr_make_int(-1); }
    if(!data) data = "";
    if(len < 0) len = (int)strlen(data);
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        gc_enter_native_block();
        ssize_t n = sendto(o->fd, data, (size_t)len, flags,
                           (struct sockaddr*)&addr, sizeof(addr));
        gc_leave_native_block();
        if(n < 0) { sock_error_code("sendTo", NET_ERR_SENDTO); return lumyr_make_int(-1); }
        return lumyr_make_int((int)n);
    } else {
        struct sockaddr_in addr;
        int rc = build_inet(host ? host : "127.0.0.1", port, &addr);
        if (rc != 0) {
            char buf[256];
            if (rc == -2) {
                snprintf(buf, sizeof(buf), "sendTo() DNS 解析超时: %s", host ? host : "");
                runtime_error_code(NET_ERR_ADDR_TIMEOUT, "SocketError", buf);
            } else {
                snprintf(buf, sizeof(buf), "sendTo() 无法解析地址: %s", host ? host : "");
                runtime_error_code(NET_ERR_ADDR_RESOLVE, "SocketError", buf);
            }
            return lumyr_make_int(-1);
        }
        gc_enter_native_block();
        ssize_t n = sendto(o->fd, data, (size_t)len, flags,
                           (struct sockaddr*)&addr, sizeof(addr));
        gc_leave_native_block();
        if(n < 0) { sock_error_code("sendTo", NET_ERR_SENDTO); return lumyr_make_int(-1); }
        return lumyr_make_int((int)n);
    }
}

Value lumyr_socket_recvfrom(Value v, int maxLen, int flags) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "recvFrom() 仅适用于 socket 对象"); return val_array(0); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "recvFrom() 套接字无效或已关闭"); return val_array(0); }
    if(maxLen <= 0) maxLen = 4096;
    char* buf = (char*)malloc((size_t)maxLen + 1);
    if(!buf) { runtime_error_code(NET_ERR_NO_MEMORY, "SocketError", "recvFrom() 内存不足"); return val_array(0); }
    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    /* 阻塞接收期间标记 GC 安全点（同 sleep），错误处理在 leave 之后 */
    gc_enter_native_block();
    ssize_t n = recvfrom(o->fd, buf, (size_t)maxLen, flags,
                         (struct sockaddr*)&src, &srclen);
    gc_leave_native_block();
    if(n < 0) {
        free(buf);
        sock_error_code("recvFrom", NET_ERR_RECVFROM);
        return val_array(0);
    }
    buf[n] = '\0';
    Value data = lumyr_make_string(buf);
    free(buf);
    /* 构造地址信息字符串 */
    char addrbuf[256];
    addrbuf[0] = '\0';
    if(src.ss_family == AF_INET) {
        struct sockaddr_in* s = (struct sockaddr_in*)&src;
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
        snprintf(addrbuf, sizeof(addrbuf), "%s:%d", ip, ntohs(s->sin_port));
    } else if(src.ss_family == AF_INET6) {
        struct sockaddr_in6* s = (struct sockaddr_in6*)&src;
        char ip[INET6_ADDRSTRLEN];
        inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
        snprintf(addrbuf, sizeof(addrbuf), "[%s]:%d", ip, ntohs(s->sin6_port));
    } else if(src.ss_family == AF_UNIX) {
        struct sockaddr_un* s = (struct sockaddr_un*)&src;
        snprintf(addrbuf, sizeof(addrbuf), "%s", s->sun_path);
    }
    /* 返回 [data, addrInfo] */
    Value arr = val_array(2);
    arr.v.array->items[0] = data;
    arr.v.array->items[1] = lumyr_make_string(addrbuf);
    return arr;
}

Value lumyr_socket_close(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "close() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return val_none();
    if(!o->closed && o->fd >= 0) {
        close(o->fd);
        o->closed = 1;
        o->fd = -1;
        o->is_connected = 0;
    }
    return val_none();
}

Value lumyr_socket_set_option(Value v, const char* name, Value val) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "setOption() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "setOption() 套接字无效或已关闭"); return val_none(); }
    if(!name) { runtime_error_code(NET_ERR_EMPTY_NAME, "SocketError", "setOption(name, val) 选项名为空"); return val_none(); }
    /* 通用 int 值提取（bool/int 均按整数处理） */
    int ival = 0;
    if(val.type == VAL_BOOL) ival = val.v.b ? 1 : 0;
    else if(val.type == VAL_INT) ival = val.v.i;
    else if(val.type == VAL_INT64) ival = (int)val.v.i64;
    else if(val.type == VAL_DOUBLE) ival = (int)val.v.d;
    else { runtime_error_code(NET_ERR_INVALID_VALUE, "SocketError", "setOption() 值需为 int/bool"); return val_none(); }

    if(strcmp(name, "nonBlock") == 0) {
        int flags = fcntl(o->fd, F_GETFL, 0);
        if(ival) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
        if(fcntl(o->fd, F_SETFL, flags) != 0) { sock_error_code("setOption(nonBlock)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "reuseAddr") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) { sock_error_code("setOption(reuseAddr)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "reusePort") == 0) {
#ifdef SO_REUSEPORT
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) { sock_error_code("setOption(reusePort)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
#else
        runtime_error_code(NET_ERR_UNSUPPORTED, "SocketError", "setOption(reusePort) 当前平台不支持");
        return val_none();
#endif
    }
    if(strcmp(name, "broadcast") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) { sock_error_code("setOption(broadcast)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "keepAlive") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) { sock_error_code("setOption(keepAlive)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "sndBuf") == 0) {
        int sz = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) != 0) { sock_error_code("setOption(sndBuf)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "rcvBuf") == 0) {
        int sz = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) != 0) { sock_error_code("setOption(rcvBuf)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "recvTimeout") == 0) {
        /* 双模式语义：阻塞模式走 SO_RCVTIMEO（内核在 recv syscall 内等超时）；
         * 协程模式（reactor+协程，fd 已非阻塞）SO_RCVTIMEO 不参与，
         * 由 SocketObj.recv_timeout_ms 字段走 reactor 定时器（co_wait_fd_timeout）。
         * 两者同抛 NET_ERR_RECV_TIMEOUT，上层分派一致。 */
        if (ival < 0) ival = 0;
        o->recv_timeout_ms = ival;
        struct timeval tv;
        tv.tv_sec = ival / 1000;
        tv.tv_usec = (ival % 1000) * 1000;
        if(setsockopt(o->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) { sock_error_code("setOption(recvTimeout)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "sendTimeout") == 0) {
        /* 双模式语义同 recvTimeout：阻塞走 SO_SNDTIMEO，协程走 send_timeout_ms 字段 */
        if (ival < 0) ival = 0;
        o->send_timeout_ms = ival;
        struct timeval tv;
        tv.tv_sec = ival / 1000;
        tv.tv_usec = (ival % 1000) * 1000;
        if(setsockopt(o->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) { sock_error_code("setOption(sendTimeout)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "tcpNoDelay") == 0) {
        int on = ival;
        if(setsockopt(o->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) { sock_error_code("setOption(tcpNoDelay)", NET_ERR_SET_OPTION); return val_none(); }
        return val_none();
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "setOption() 未知选项: %s", name);
    runtime_error_code(NET_ERR_UNKNOWN_OPTION, "SocketError", buf);
    return val_none();
}

Value lumyr_socket_get_option(Value v, const char* name) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "getOption() 仅适用于 socket 对象"); return lumyr_make_int(0); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "getOption() 套接字无效或已关闭"); return lumyr_make_int(0); }
    if(!name) return lumyr_make_int(0);
    if(strcmp(name, "nonBlock") == 0) {
        int flags = fcntl(o->fd, F_GETFL, 0);
        return lumyr_make_bool((flags & O_NONBLOCK) ? 1 : 0);
    }
    int rv = 0;
    socklen_t rl = sizeof(rv);
    if(strcmp(name, "reuseAddr") == 0) {
        if(getsockopt(o->fd, SOL_SOCKET, SO_REUSEADDR, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    if(strcmp(name, "broadcast") == 0) {
        if(getsockopt(o->fd, SOL_SOCKET, SO_BROADCAST, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    if(strcmp(name, "keepAlive") == 0) {
        if(getsockopt(o->fd, SOL_SOCKET, SO_KEEPALIVE, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    if(strcmp(name, "sndBuf") == 0) {
        if(getsockopt(o->fd, SOL_SOCKET, SO_SNDBUF, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    if(strcmp(name, "rcvBuf") == 0) {
        if(getsockopt(o->fd, SOL_SOCKET, SO_RCVBUF, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    if(strcmp(name, "tcpNoDelay") == 0) {
        if(getsockopt(o->fd, IPPROTO_TCP, TCP_NODELAY, &rv, &rl) != 0) return lumyr_make_int(0);
        return lumyr_make_int(rv);
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "getOption() 未知选项: %s", name);
    runtime_error_code(NET_ERR_UNKNOWN_OPTION, "SocketError", buf);
    return lumyr_make_int(0);
}

Value lumyr_socket_fileno(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "fileno() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return lumyr_make_int(-1);
    return lumyr_make_int(o->closed ? -1 : o->fd);
}
