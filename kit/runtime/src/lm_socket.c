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
#include "lm_blocking_pool.h" /* blocking 池：getaddrinfo 流放，调度线程不阻塞 */
#include "lm_timer.h"        /* DNS 超时仲裁（timeout>0 时） */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
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
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
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

// 报错包装：拼 errno 文本并 runtime_error_code（带操作对应的枚举码）。
// 调用点均紧跟失败的系统调用，errno 仍有效：随错误对象携带 os_errno 快照，
// 上层 e.getErrno() 编程判定资源耗尽等类别（不做消息文本匹配）。
static void sock_error_code(const char* op, NetErrorCode code) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s() 失败: %s", op, strerror(errno));
    runtime_error_code_errno(code, errno, "SocketError", buf);
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

/* Phase 8.14：标记当前协程 IO 亲和。
 * pinned 语义扩展：原义仅"本协程正在等 fd、唤醒须回本线程"；扩展为
 * "任何在协程内使用 per-thread reactor 网络栈的协程"——其发起的 fd 注册
 * 挂在本线程 reactor、唤醒经本线程 scheduler，这些上下文无法随协程
 * 迁移带走（与 Go 全局 netpoll 不同）。一旦置位永久不可被 sysmon
 * 强制迁移：否则协程落到无 reactor 的 compute 线程后，co_wait_fd 立即
 * 失败、IO 静默降级阻塞路径，造成批量超时与结果错配。
 * 调用点：connect/accept/send/recv 的协程入口 + close 通知。
 * 无协程或本线程无 reactor（thread-per-conn / compute 线程）时不置位。 */
static void socket_mark_io_affine(void) {
    lm_co_t* co = lm_co_current();
    if (co && g_socket_reactor) co->pinned = 1;
}

/* Phase 8.8：fd 等待三态单字仲裁（就绪 / 超时 / 关闭 三方抢同一原子字）。
 *
 * 模型：等待协程把自身指针 CAS 进 conn 的 rg/wg 字（NIL→WAITING），之后三方
 * 以 CAS WAITING→X 抢唯一唤醒权：
 *   - 事件就绪 fd_wait_read_cb/fd_wait_write_cb（reactor 线程）→ LM_FD_READY
 *   - 超时    fd_wait_timeout_cb（timer 线程，waitCtx 自带方向与 co 快照）→ LM_FD_TIMEOUT
 *   - 关闭    lumyr_socket_close 路径（协程线程）→ LM_FD_CLOSED
 * 赢 CAS 者无条件唤醒协程，输家 no-op——协程只被唤醒一次，无重复 resume /
 * 双不唤醒（与 DNS 仲裁模型一致：CAS 成功本身即"等待仍当前"的完整证明，
 * 因为协程只在被唤醒后才可能 cleanup/recycle conn）。
 * fd_generation：注册时快照进 wait_gen_r/w 与 waitCtx，回调 CAS 前预检——
 * 不一致说明 conn 已经 get/free/close 变迁（slot 复用），陈旧回调不碰等待字。
 * 状态迁移无条件 CAS，唤醒才可条件（内存序 acq_rel/acquire 保证快照与 sched
 * 对赢家可见）。
 *
 * waitCtx 所有权（与 lm_timer_cancel 返回值语义对齐）：
 *   - 协程 cleanup：cancel 返回 0（回调保证不运行）→ 协程释放；
 *     返回 1（running）/ -1（已执行）→ 回调持有/已释放，协程不得再碰。
 *   - 超时回调：一旦运行即独占，无论 CAS 输赢最后都 free。 */
typedef struct {
    lm_connection_t* conn;
    _Atomic uintptr_t* word;    /* 本次等待的方向字（rg 或 wg） */
    lm_co_t* co;                /* 等待协程快照（不读 conn->co——复用后会被覆写） */
    uint64_t gen;               /* 注册时的 fd_generation 快照 */
} FdWaitCtx;

/* 事件回调共用逻辑（reactor 线程）：CAS WAITING→READY，赢家投递协程。 */
static void fd_wait_fire_ready(lm_connection_t* conn, _Atomic uintptr_t* word,
                               uint64_t wait_gen, void* data) {
    lm_co_t* co = (lm_co_t*)data;
    if (!co) return;
    uintptr_t expected = (uintptr_t)co;
    int won = atomic_compare_exchange_strong_explicit(word, &expected, LM_FD_READY,
            memory_order_acq_rel, memory_order_acquire);
    if (won) {
        /* 赢了仲裁：gen 新鲜才唤醒（保险比对——单线程 reactor 下事件回调与
         * 等待协程 cleanup 串行，正常必新鲜；跨场景复用时防陈旧事件误唤醒） */
        if (wait_gen == atomic_load_explicit(&conn->fd_generation, memory_order_acquire)) {
            if (conn->sched) {
                lm_scheduler_post_local(conn->sched, co);
            } else {
                lm_co_resume(co);  /* 无 scheduler：同线程串行安全（Phase 5 遗留路径） */
            }
        }
    }
    /* CAS 输了：超时/关闭方已抢先，no-op */
}

static void fd_wait_read_cb(lm_connection_t* conn, uint32_t events, void* data) {
    (void)events;
    fd_wait_fire_ready(conn, &conn->rg, conn->wait_gen_r, data);
}

static void fd_wait_write_cb(lm_connection_t* conn, uint32_t events, void* data) {
    (void)events;
    fd_wait_fire_ready(conn, &conn->wg, conn->wait_gen_w, data);
}

/* 超时回调（timer 线程）：CAS WAITING→TIMEOUT，赢家投递协程。
 * gen 仅作 CAS 前预检（陈旧定时器不干预复用后的等待字）；
 * 赢 CAS 后无条件唤醒——CAS 成功即"等待仍当前"的证明（协程只在被唤醒后
 * 才可能 recycle conn；co 地址 ABA 场景由 gen 预检挡住）。
 * waitCtx 一旦运行即由本回调独占释放（无论输赢）。 */
static void fd_wait_timeout_cb(lm_timer_id_t timer_id, void* data) {
    (void)timer_id;
    FdWaitCtx* ctx = (FdWaitCtx*)data;
    lm_connection_t* conn = ctx->conn;
    /* 代次预检：conn 已经 get/free/close 变迁（slot 复用）→ 陈旧定时器，
     * 不碰等待字（字内可能是复用后新等待者的注册） */
    if (ctx->gen == atomic_load_explicit(&conn->fd_generation, memory_order_acquire)) {
        uintptr_t expected = (uintptr_t)ctx->co;
        if (atomic_compare_exchange_strong_explicit(ctx->word, &expected, LM_FD_TIMEOUT,
                memory_order_acq_rel, memory_order_acquire)) {
            /* 赢仲裁：无条件唤醒（无 scheduler 时降级不 wake，与旧语义一致） */
            if (conn->sched) {
                lm_scheduler_wakeup(conn->sched, ctx->co);  /* 跨线程：post + self-pipe */
            }
        }
        /* CAS 输了：事件/关闭方已抢先，no-op */
    }
    free(ctx);
}

/* 协程挂起等 fd 事件（带超时）：三态单字仲裁版。
 * 调用点均为单方向等待（connect 写 / accept 读 / send 写 / recv 读），
 * want_read 优先选 rg 字，否则 wg 字；两方向同传属未定义（本路径不支持）。
 * 返回 0 事件就绪 / 1 超时 / -1 失败或 fd 已关闭。
 * 必须在协程内调用（lm_co_current() != NULL）。
 * 流程：get_connection → gen 快照 → CAS NIL→WAITING(co) → 注册事件 →
 * 注册集中定时器（waitCtx 带方向）→ yield → resume 后按字终态区分 →
 * 摘事件（CLOSED 时 fd 已死，del 返回 EBADF 忽略）→ 摘定时器（已触发时
 * cancel no-op）→ 归还连接池。 */

static int co_wait_fd_timeout(int fd, int want_read, int want_write, int timeout_ms) {
    lm_co_t* co = lm_co_current();
    if (!co || !g_socket_reactor) return -1;
    /* Phase 8.2：fd 等待自动置 pinned——本协程的 IO 事件挂在当前线程 reactor 上，
     * 唤醒必须回到本线程的 scheduler（scheduler post 分流依据，见 lm_co.h）。
     * 置位后该协程永不入 WSQ / 不被窃取，跨线程 wakeup 走 mutex 定向队列。 */
    co->pinned = 1;
    lm_connection_t* conn = lm_reactor_get_connection(g_socket_reactor, fd);
    if (!conn) return -1;
    /* Phase 8.8：选方向字（调用点均单方向），快照 fd_generation 供回调比对 */
    _Atomic uintptr_t* word = want_read ? &conn->rg : &conn->wg;
    uint64_t gen = atomic_load_explicit(&conn->fd_generation, memory_order_acquire);
    if (want_read) conn->wait_gen_r = gen; else conn->wait_gen_w = gen;
    /* 登记回调：事件走 fd_wait_*_cb 三态仲裁；data 仍传 co（回调仲裁用）。
     * conn->sched 记录所属 scheduler，超时回调（timer 线程跨线程）据此 wakeup。 */
    conn->read_handler  = want_read  ? fd_wait_read_cb  : NULL;
    conn->write_handler = want_write ? fd_wait_write_cb : NULL;
    conn->read_data  = co;
    conn->write_data = co;
    conn->co = co;
    conn->sched = lm_scheduler_get_current();
    uint32_t events = 0;
    if (want_read)  events |= LM_EVENT_READ;
    if (want_write) events |= LM_EVENT_WRITE;
    /* CAS NIL→WAITING(co)：占住等待字，三方仲裁的起点。
     * 失败（理论上不可能，get_connection 刚置 NIL）→ 归还并报错。 */
    uintptr_t expected = LM_FD_NIL;
    if (!atomic_compare_exchange_strong_explicit(word, &expected, (uintptr_t)co,
            memory_order_acq_rel, memory_order_acquire)) {
        lm_reactor_free_connection(g_socket_reactor, conn);
        return -1;
    }
    /* Phase 8.8：注册等待字回溯，供协程强制 destroy 时安全解仲裁。
     * Phase 8.13：同步登记等待源类别（sysmon stuck 检测）——
     * 顺序：先等待指针字段、最后 store-release wait_kind（sysmon acquire
     * 读 kind 后必见指针字段）；清除顺序相反（先清指针、最后清 kind）。 */
    atomic_store_explicit(&co->waiting_word, word, memory_order_release);
    co->waiting_conn = conn;
    atomic_store_explicit(&co->wait_kind, LM_WAIT_FD, memory_order_release);
    if (lm_reactor_add(g_socket_reactor, conn, events) != 0) {
        /* add 失败：CAS WAITING→NIL 回滚（此时三方尚未注册，必赢）再归还 */
        uintptr_t exp = (uintptr_t)co;
        atomic_compare_exchange_strong_explicit(word, &exp, LM_FD_NIL,
                memory_order_acq_rel, memory_order_acquire);
        atomic_store_explicit(&co->waiting_word, NULL, memory_order_release);
        atomic_store_explicit(&co->wait_kind, LM_WAIT_NONE, memory_order_release);
        lm_reactor_free_connection(g_socket_reactor, conn);
        return -1;
    }
    FdWaitCtx* waitCtx = NULL;
    lm_timer_id_t timer_id = LM_TIMER_INVALID_ID;
    if (timeout_ms > 0) {
        waitCtx = (FdWaitCtx*)malloc(sizeof(FdWaitCtx));
        if (waitCtx) {
            waitCtx->conn = conn;
            waitCtx->word = word;
            waitCtx->co = co;
            waitCtx->gen = gen;
            timer_id = lm_reactor_add_timer(g_socket_reactor, (uint64_t)timeout_ms,
                                            fd_wait_timeout_cb, waitCtx);
            if (timer_id == LM_TIMER_INVALID_ID) {
                free(waitCtx);
                waitCtx = NULL;
            }
        }
        if (timer_id == LM_TIMER_INVALID_ID) {
            /* 定时器不可用：摘事件 + CAS WAITING→NIL 回滚 + 归还 */
            lm_reactor_del(g_socket_reactor, conn, conn->active_events);
            uintptr_t exp = (uintptr_t)co;
            atomic_compare_exchange_strong_explicit(word, &exp, LM_FD_NIL,
                    memory_order_acq_rel, memory_order_acquire);
            atomic_store_explicit(&co->waiting_word, NULL, memory_order_release);
            atomic_store_explicit(&co->wait_kind, LM_WAIT_NONE, memory_order_release);
            lm_reactor_free_connection(g_socket_reactor, conn);
            return -1;
        }
    }
    /* 等待循环：以等待字为唯一真相之源，只有仲裁赢家写入的
     * READY/TIMEOUT/CLOSED 是终态。resume 回来字仍是 WAITING(co) = 伪唤醒
     * （调度层既有双投递：spawn 入就绪队列 + 业务层显式 resume，drain 会把
     * 队列里的陈旧条目再 resume 一次——见 vm_builtin.c BUILTIN_CO_SPAWN 段
     * 注释；事件注册与定时器仍在位，无需重注册，直接再 yield 继续等）。 */
    uintptr_t state;
    for (;;) {
        lm_co_yield();  /* 切回 reactor 主循环等事件/超时/关闭 */
        state = atomic_load_explicit(word, memory_order_acquire);
        if (state != (uintptr_t)co) break;  /* 仲裁赢家写过 → 终态 */
    }
    /* 清理（协程续行负责）：摘事件（CLOSED 时 fd 已被 close()，del 返回
     * EBADF/ENOENT 忽略——内核已自动摘除）；摘定时器——waitCtx 所有权随
     * cancel 返回值移交：0（回调保证不运行）→ 协程释放；1/-1（运行中/已
     * 执行）→ 回调持有或已释放，协程不得再碰（防 UAF / double-free）。 */
    lm_reactor_del(g_socket_reactor, conn, conn->active_events);
    if (timer_id != LM_TIMER_INVALID_ID) {
        if (lm_timer_cancel(timer_id) == 0) {
            free(waitCtx);
        }
    }
    /* Phase 8.8：清理等待字回溯（协程已恢复，无 destroy 解仲裁需求）。
     * Phase 8.13：先清指针字段、最后清类别（与登记顺序相反）——sysmon
     * 读到 kind==FD 时 waiting_word 必仍有效或为 NULL（NULL 则跳过本轮）。 */
    atomic_store_explicit(&co->waiting_word, NULL, memory_order_release);
    co->waiting_conn = NULL;
    atomic_store_explicit(&co->wait_kind, LM_WAIT_NONE, memory_order_release);
    lm_reactor_free_connection(g_socket_reactor, conn);
    return (state == LM_FD_READY) ? 0 : (state == LM_FD_TIMEOUT) ? 1 : -1;
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

/* ============================================================
 * 协程路径：getaddrinfo 流放 blocking 池，调度线程不阻塞
 * ============================================================
 * 仲裁模型：getaddrinfo 不可中断（见文件头既有结论），故"完成"与"超时"
 * 两个事件各自 CAS 抢占 ctx.state（0→1 完成 / 0→2 超时），仅赢家回投协程
 * → 协程只会被唤醒一次，无重复 resume / 丢唤醒。
 * 超时后 getaddrinfo 仍在 blocking 线程跑完（结果丢弃），与旧超时路径一致。
 * ctx 引用计数：blocking 完成回调、超时定时器、协程各持一份，
 * 最后一份释放时销毁——保证协程读结果期间 ctx 不被提前 free。
 * scheduler 引用：提交时捕获当前 scheduler 并 retain，协程释放 ctx 后 release
 * （回投动作完成前 scheduler 不会被销毁）。 */
typedef struct {
    char*               host;
    lm_co_t*            co;
    lm_scheduler_t*     sched;
    struct sockaddr_in  addr;    /* 解析结果（完成方写入） */
    int                 rc;      /* 0 成功 / -1 失败 */
    _Atomic int         state;   /* 0 等待 / 1 完成 / 2 超时 */
    _Atomic int         refcnt;
} lm_dns_co_ctx;

static void dns_co_release(lm_dns_co_ctx* c) {
    if (atomic_fetch_sub_explicit(&c->refcnt, 1, memory_order_acq_rel) == 1) {
        free(c->host);
        free(c);
    }
}

/* blocking 池任务：在独立池线程跑 getaddrinfo，结果写入 ctx。 */
static void* dns_co_blocking(void* arg) {
    lm_dns_co_ctx* c = (lm_dns_co_ctx*)arg;
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    int rc = -1;
    if (getaddrinfo(c->host, NULL, &hints, &res) == 0 && res) {
        memcpy(&c->addr, res->ai_addr, sizeof(struct sockaddr_in));
        rc = 0;
        freeaddrinfo(res);
    }
    c->rc = rc;
    return c;
}

/* blocking 完成回调（池线程）：CAS 抢"完成"，赢则回投协程。 */
static void dns_co_done(lm_co_t* co, void* result) {
    lm_dns_co_ctx* c = (lm_dns_co_ctx*)result;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&c->state, &expected, 1,
            memory_order_acq_rel, memory_order_acquire)) {
        lm_scheduler_wakeup(c->sched, co);
    }
    dns_co_release(c);
}

/* 超时回调（timer 线程）：CAS 抢"超时"，赢则回投协程。回调不阻塞。 */
static void dns_co_timeout(lm_timer_id_t id, void* arg) {
    (void)id;
    lm_dns_co_ctx* c = (lm_dns_co_ctx*)arg;
    int expected = 0;
    if (atomic_compare_exchange_strong_explicit(&c->state, &expected, 2,
            memory_order_acq_rel, memory_order_acquire)) {
        lm_scheduler_wakeup(c->sched, c->co);
    }
    dns_co_release(c);
}

/* 协程版 build_inet：返回 0 成功 / -1 解析失败 / -2 超时。
 * 提交失败/OOM 时回退旧 build_inet（可能阻塞调度线程，保证功能可用）。 */
static int build_inet(const char* host, int port, struct sockaddr_in* addr);

static int build_inet_co(const char* host, int port, struct sockaddr_in* addr,
                         int timeout) {
    lm_co_t* co = lm_co_current();
    lm_scheduler_t* sched = lm_scheduler_get_current();
    if (!co || !sched) return build_inet(host, port, addr);   /* 保险：调用方已判上下文 */
    lm_dns_co_ctx* c = (lm_dns_co_ctx*)calloc(1, sizeof(lm_dns_co_ctx));
    if (!c) return build_inet(host, port, addr);
    c->host = strdup(host);
    if (!c->host) { free(c); return build_inet(host, port, addr); }
    c->co = co;
    c->sched = sched;
    /* 初始引用：完成回调 + 协程；超时>0 再加定时器一份。 */
    atomic_init(&c->state, 0);
    atomic_init(&c->refcnt, timeout > 0 ? 3 : 2);
    lm_scheduler_retain(sched);
    if (lm_blocking_submit(dns_co_blocking, c, dns_co_done, co) != 0) {
        lm_scheduler_release(sched);
        free(c->host);
        free(c);
        return build_inet(host, port, addr);
    }
    if (timeout > 0) {
        /* 绝对到期 = 当前单调 ms + timeout；lm_now_ns 与 timer 同为 CLOCK_MONOTONIC。 */
        uint64_t deadline = (uint64_t)(lm_now_ns() / 1000000) + (uint64_t)timeout;
        if (lm_timer_add(deadline, dns_co_timeout, c) == LM_TIMER_INVALID_ID) {
            /* 定时器不可用：降级为无超时，撤回定时器引用（仲裁中不会再出现）。 */
            atomic_fetch_sub_explicit(&c->refcnt, 1, memory_order_acq_rel);
        }
    }
    lm_co_yield();
    /* 唤醒续行：唯一 CAS 赢家保证只到这里一次。 */
    int st = atomic_load_explicit(&c->state, memory_order_acquire);
    int result = -1;
    if (st == 1 && c->rc == 0) {
        memcpy(addr, &c->addr, sizeof(*addr));
        addr->sin_port = htons((uint16_t)port);
        result = 0;
    } else if (st == 2) {
        result = -2;
    }
    dns_co_release(c);
    lm_scheduler_release(sched);
    return result;
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
    /* 协程上下文：getaddrinfo 流放 blocking 池 + yield，避免冻结调度线程
     * （connect/bind/sendto 均经由本函数）；非协程（如主线程启动建 server）
     * 保持原同步 / 自管线程路径。 */
    if (lm_co_current() != NULL && lm_scheduler_get_current() != NULL) {
        return build_inet_co(host, port, addr, g_dns_timeout_ms);
    }
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

/* 读取系统全连接队列上限 somaxconn：listen(fd, backlog) 超过该值会被内核静默
 * 截断，而队列满时新连接在 macOS 被直接 RST、Linux 被丢弃 SYN——表象是高并发
 * 突发下零星 "connect reset by peer"，极易误判为应用拒绝。读出供 listen 告警；
 * 不支持的平台/读取失败返回 -1（跳过，不影响行为）。 */
static int socket_sys_somaxconn(void) {
#if defined(__linux__)
    FILE* fp = fopen("/proc/sys/net/core/somaxconn", "r");
    if (!fp) return -1;
    int v = -1;
    if (fscanf(fp, "%d", &v) != 1) v = -1;
    fclose(fp);
    return v;
#elif defined(__APPLE__)
    int v = -1;
    size_t len = sizeof(v);
    if (sysctlbyname("kern.ipc.somaxconn", &v, &len, NULL, 0) != 0) return -1;
    return v;
#else
    return -1;
#endif
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
    /* backlog 被系统上限静默截断时显式告警（不改变行为，仅消除隐性坑）：
     * 队列满的溢出连接由内核处置，应用层无法拦截，只能调大 somaxconn 或错峰。 */
    int cap = socket_sys_somaxconn();
    if (cap > 0 && backlog > cap) {
        fprintf(stderr,
            "[warn] listen backlog=%d 超过系统全连接队列上限 somaxconn=%d，已被内核截断；"
            "瞬时并发突发超出该上限的连接会被 macOS 直接重置（Linux 丢弃 SYN），表现为零星 "
            "\"connect reset by peer\"。请调大系统参数（macOS: sysctl -w kern.ipc.somaxconn=N；"
            "Linux: net.core.somaxconn）或让客户端错峰接入。/ listen backlog=%d exceeds OS "
            "somaxconn=%d and was clamped; overflow burst connections are reset (macOS) or "
            "SYN-dropped (Linux). Raise somaxconn or stagger connects.\n",
            backlog, cap, backlog, cap);
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
        /* fd 耗尽（EMFILE/ENFILE）细分专用码：accept 资源耗尽类退避需编程
         * 判定（对标 nginx ngx_event_accept.c EMFILE 处理），不做文本匹配 */
        if(errno == EMFILE || errno == ENFILE) {
            sock_error_code("accept", NET_ERR_FD_EXHAUSTED);
        } else {
            sock_error_code("accept", NET_ERR_ACCEPT);
        }
        return val_none();
    }
    /* 新客户端套接字，类型与服务端一致，已连接 */
    return lumyr_socket_from_fd(cfd, (int)o->kind, 1);
}

/* ============================================================
 * T5：单 acceptor + 应用层 RR 分派
 * ============================================================
 * 交接不变量：acceptor 只产出裸 fd 投到目标 reactor 的 MPSC 入站队列；
 * fd 在 owner receiver pop 并 wrap/ADD 前不挂任何 epoll/kqueue；
 * 拒收 close 全部在 owner 线程发生。
 *
 * 关键生命周期约束：不能把 retained 的目标 reactor 指针跨 co_wait_fd 的
 * yield 持有——被强毁的挂起协程直接释放 fcontext 栈、不走 C 栈展开，
 * 跨 yield 的 release 不会执行（reactor 引用随每次 RR 启停泄漏）。
 * 故"选 worker"只产出 idx，retain 收紧到 accept 之后的提交瞬间。 */

/* RR 选择游标（进程内单 listener 语义；多 listener 共享只影响分布均匀性）。 */
static _Atomic unsigned g_rr_cursor = 0;

/* 判定某 worker 当前是否合格（权威占用 reserved 未达上限且入站队列未满）。
 * 调用时 t 已 retain；本函数不 release。 */
static int rr_worker_fits(lm_reactor_t* t, int maxConn) {
    if (lm_reactor_inbound_pending(t) >= lm_reactor_inbound_capacity(t)) return 0;
    if (maxConn > 0 && lm_reactor_inbound_reserved(t) >= maxConn) return 0;
    return 1;
}

/* 从游标起选第一个合格 worker：只返回 idx（瞬时 retain 做读检查后即释放，
 * 不跨 yield）；全部高压或注册表未就绪返回 -1。 */
static int rr_pick_idx(int nworkers, int maxConn) {
    unsigned cur = atomic_load_explicit(&g_rr_cursor, memory_order_relaxed);
    for (int k = 0; k < nworkers; k++) {
        int idx = ((int)cur + k) % nworkers;
        lm_reactor_t* t = lm_reactor_by_idx_retained(idx);
        if (!t) continue;
        int fits = rr_worker_fits(t, maxConn);
        lm_reactor_release(t);
        if (fits) return idx;
    }
    return -1;
}

/* accept 已得 cfd 后提交：先试首选，满则按 RR 序换其余 worker（每个只在
 * 锁内 retain 到提交完成）。返回 0 已被某 worker 接管；-1 全部失败
 *（调用方 close cfd；fd 不挂任何 epoll，close 安全）。 */
static int rr_submit_or_fail(int chosen, int nworkers, int cfd, int kind,
                             const struct sockaddr* addr, socklen_t addrlen,
                             int maxConn) {
    for (int k = 0; k < nworkers; k++) {
        int idx = (chosen + k) % nworkers;
        lm_reactor_t* t = lm_reactor_by_idx_retained(idx);
        if (!t) continue;
        int rc = lm_reactor_submit_fd(t, cfd, kind, addr, addrlen, maxConn);
        lm_reactor_release(t);
        if (rc == 0) return 0;
        if (rc == -2) return -1;   /* 参数/分配错误：重试无意义 */
        /* rc == -1 容量/队列满：换下一个 worker */
    }
    return -1;
}

int lumyr_socket_accept_rr(Value v, int nworkers, int maxConn) {
    if(v.type != VAL_SOCKET) {
        runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError",
            "acceptRr() 仅适用于 socket 对象 / acceptRr: socket object required");
        return -3;
    }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) {
        runtime_error_code(NET_ERR_BAD_STATE, "SocketError",
            "acceptRr() 监听套接字无效或已关闭 / acceptRr: invalid or closed listener");
        return -3;
    }
    if (nworkers < 1) return -3;
    /* 注册表未满：worker reactor 尚未全部就绪，让出等待（启动屏障）。 */
    if (lm_reactor_registry_count() < nworkers) return 2;
    lm_co_t* co = lm_co_current();
    if (!co || !g_socket_reactor) {
        runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError",
            "acceptRr() 必须在 worker 协程内调用 / acceptRr: must run inside a worker coroutine");
        return -3;
    }
    socket_mark_io_affine();
    set_nonblock(o->fd);
    /* 先选 worker 再 accept：全高压时不 accept，listener 保持 ET 未读状态，
     * 连接留在内核 backlog（全满停 accept 的自然反压语义）。 */
    int chosen = rr_pick_idx(nworkers, maxConn);
    if (chosen < 0) return 1;
    atomic_store_explicit(&g_rr_cursor, (unsigned)(chosen + 1) % (unsigned)nworkers,
                          memory_order_relaxed);
    struct sockaddr_storage cli;
    socklen_t clilen = sizeof(cli);
    int cfd;
    for (;;) {
        cfd = accept(o->fd, (struct sockaddr*)&cli, &clilen);
        if (cfd >= 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            /* ET 无更多连接：挂 listener 读事件等就绪后重试。
             * 此处不持有任何目标 reactor 引用（见函数头生命周期约束）。 */
            if (co_wait_fd(o->fd, 1, 0) != 0) return -2;
            continue;
        }
        if (errno == EMFILE || errno == ENFILE || errno == ENOMEM) return -1;
        if (errno == EINTR) continue;          /* 信号中断：内部重试，不产生瞬时报错 */
        if (errno == EBADF || errno == EINVAL) return -3;
        return -2;
    }
    /* 新 cfd 设非阻塞 + CLOEXEC（与 lumyr_socket_accept 同口径，跨平台）。 */
    {
        int fl = fcntl(cfd, F_GETFL, 0);
        if (fl != -1) fcntl(cfd, F_SETFL, fl | O_NONBLOCK);
        int fd = fcntl(cfd, F_GETFD, 0);
        if (fd != -1) fcntl(cfd, F_SETFD, fd | FD_CLOEXEC);
    }
    if (rr_submit_or_fail(chosen, nworkers, cfd, (int)o->kind,
                          (struct sockaddr*)&cli, clilen, maxConn) == 0) {
        return 0;
    }
    /* 全部 worker 队列竞态满：close 并反压（等价一次拒绝，下一连接重试）。 */
    close(cfd);
    return 1;
}

Value lumyr_reactor_recv_inbound(lm_reactor_t* r) {
    if (!r) {
        runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError",
            "recvInbound 需要 reactor 实例 / recvInbound: reactor instance required");
        return val_none();
    }
    lm_co_t* co = lm_co_current();
    if (!co) {
        runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError",
            "recvInbound 必须在协程内调用 / recvInbound: must run inside a coroutine");
        return val_none();
    }
    /* receiver 全程 pinned：fd 事件挂本线程 reactor，唤醒必须回本线程。 */
    co->pinned = 1;
    for (;;) {
        lm_inbound_t msg;
        if (lm_reactor_inbound_pop(r, &msg)) {
            /* owner 线程 wrap：SocketObj GC 对象在本线程分配，
             * 随后 onAccepted 路径与普通 accept 完全一致。 */
            return lumyr_socket_from_fd(msg.fd, msg.kind, 1);
        }
        lm_scheduler_t* sched = lm_scheduler_get_current();
        if (lm_reactor_inbound_arm(r, co, sched)) {
            /* 已登记等待者：提交方入队时摘走并经 scheduler 唤醒。 */
            lm_co_yield();
        }
        /* arm 返回 0 = 锁内复查已有入站（提交先于登记），直接循环 pop 必中，
         * 零空转；yield 返回后同理。 */
    }
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
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if (in_co) {
        socket_mark_io_affine();
        set_nonblock(o->fd);
    }
    for (;;) {
        /* 接收 buffer 生命周期仅限单次 recv 尝试：EAGAIN 挂起前必须释放——
         * 否则 idle 连接在整个等待期间常驻 ~4KiB heap（malloc(maxLen+1)
         * 在 macOS 落入 4608B slot；曾导致每连接 +4.5KiB、idle 换出也
         * 无法回收）。resume 后下一轮重新 malloc，数据真正到达时才持有。 */
        char* buf = (char*)malloc((size_t)maxLen + 1);
        if(!buf) { runtime_error_code(NET_ERR_NO_MEMORY, "SocketError", "recv() 内存不足"); return lumyr_make_string(""); }
        ssize_t n;
        if (in_co) {
            n = recv(o->fd, buf, (size_t)maxLen, flags);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                /* 协程模式：先释放 buffer 再挂读事件，resume 后重试。
                 * per-socket recvTimeout（SocketObj 字段）走 reactor 定时器：
                 * 超时返回 1，按 NET_ERR_RECV_TIMEOUT 抛错（与阻塞模式
                 * SO_RCVTIMEO 同码，上层 HTTP 408 分派自动生效）。 */
                free(buf);
                int wr = co_wait_fd_timeout(o->fd, 1, 0, o->recv_timeout_ms);
                if (wr != 0) {
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

Value lumyr_socket_recv_into(Value v, Value buf, int flags, int maxLen) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "recvInto() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error_code(NET_ERR_BAD_STATE, "SocketError", "recvInto() 套接字无效或已关闭"); return lumyr_make_int(-1); }
    if(buf.type != VAL_BYTES) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "recvInto(buf) 参数必须是 bytes 定长缓冲 bytes(n)"); return lumyr_make_int(-1); }
    BytesObj* bo = (BytesObj*)buf.v.bytes_obj;
    if(!bo || !bo->data || bo->cap <= 0) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "recvInto(buf) 需要非空定长缓冲 bytes(n>0)"); return lumyr_make_int(-1); }
    int readCap = bo->cap;
    if(maxLen > 0 && maxLen < readCap) readCap = maxLen;  /* 精确边界：不吞掉后续报文 */
    int in_co = (lm_co_current() != NULL && g_socket_reactor != NULL);
    if (in_co) {
        socket_mark_io_affine();
        set_nonblock(o->fd);
    }
    for (;;) {
        /* 直接 recv 进复用缓冲：等待期间该缓冲常驻是复用语义本身，
         * 循环内零新分配，流式收包内存与报文总量无关。 */
        ssize_t n;
        if (in_co) {
            n = recv(o->fd, bo->data, (size_t)readCap, flags);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int wr = co_wait_fd_timeout(o->fd, 1, 0, o->recv_timeout_ms);
                if (wr != 0) {
                    if (wr == 1) {
                        runtime_error_code(NET_ERR_RECV_TIMEOUT, "SocketError", "接收超时 / socket receive timeout");
                    } else {
                        sock_error_code("recvInto", NET_ERR_RECV);
                    }
                    return lumyr_make_int(-1);
                }
                continue;
            }
        } else {
            gc_enter_native_block();
            n = recv(o->fd, bo->data, (size_t)readCap, flags);
            gc_leave_native_block();
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                runtime_error_code(NET_ERR_RECV_TIMEOUT, "SocketError", "接收超时 / socket receive timeout");
                return lumyr_make_int(-1);
            }
        }
        if(n < 0) {
            sock_error_code("recvInto", NET_ERR_RECV);
            return lumyr_make_int(-1);
        }
        if(n == 0) {
            /* 对端关闭 / EOF */
            o->is_connected = 0;
            bo->len = 0;
            return lumyr_make_int(0);
        }
        bo->len = (int)n;
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

/* Phase 8.8：close 前通知——对本线程 reactor 中正等待该 fd 的 conn 做
 * WAITING→CLOSED 仲裁，赢家唤醒等待协程（其 resume 后见 CLOSED 返回 -1，
 * 不会在死 fd 上重试 syscall）。
 * 仲裁必须在 close(fd) 之前：close 后 fd 号可能被其他线程复用，fd_map 查找
 * 会错过或错配，等待协程将挂到超时。
 * 跨线程 close（close 协程在别的 scheduler 线程）：TLS 取到的是别的 reactor
 * 或 NULL，找不到本 conn 则跳过——等待协程仍靠超时/事件路径唤醒，
 * 与既有行为一致（不退化）。 */
static void fd_wait_close_notify(int fd) {
    lm_reactor_t* r = g_socket_reactor;
    if (!r || fd < 0 || fd >= r->conn_capacity) return;
    /* 发起 close 的协程（如 IO 编排协程）同样 IO 亲和——它在 reactor 语境
     * 操作 fd，sysmon 不得将其迁走到无 reactor 的 compute 线程。 */
    socket_mark_io_affine();
    lm_connection_t* conn = r->fd_map[fd];
    if (!conn || conn->fd != fd) return;   /* 无等待者或 slot 已易主 */
    lm_co_t* co = (lm_co_t*)conn->co;
    if (!co) return;
    /* 代次 bump：使等待方注册时快照的 gen 失效（陈旧定时器不再碰等待字） */
    atomic_fetch_add_explicit(&conn->fd_generation, 1, memory_order_acq_rel);
    /* 读/写两字各试一次（等待是单方向的，只有其中一个可能处于 WAITING） */
    for (int i = 0; i < 2; i++) {
        _Atomic uintptr_t* word = (i == 0) ? &conn->rg : &conn->wg;
        uintptr_t expected = (uintptr_t)co;
        if (atomic_compare_exchange_strong_explicit(word, &expected, LM_FD_CLOSED,
                memory_order_acq_rel, memory_order_acquire)) {
            /* 赢仲裁：唤醒等待协程。本线程（close 协程与等待协程同属本
             * reactor 的 scheduler）走 post_local 快通道；conn->sched 为
             * NULL（无 scheduler 遗留路径）时直接 resume（同线程串行安全）。 */
            if (conn->sched) {
                lm_scheduler_post_local(conn->sched, co);
            } else {
                lm_co_resume(co);
            }
            return;   /* 单方向等待：一个字赢即完成 */
        }
        /* CAS 输：该字非 WAITING(co)（NIL 或已被事件/超时仲裁），试下一字 */
    }
}

Value lumyr_socket_close(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error_code(NET_ERR_INVALID_TYPE, "SocketError", "close() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return val_none();
    if(!o->closed && o->fd >= 0) {
        fd_wait_close_notify(o->fd);   /* Phase 8.8：先仲裁唤醒等待协程，再关 fd */
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
