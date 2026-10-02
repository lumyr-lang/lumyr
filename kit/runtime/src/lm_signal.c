// lm_signal.c —— 进程信号优雅退出（G1）
// 信号表驱动（nginx 范式）+ self-pipe（libuv 范式）：
//   - 处理器仅 write 1 字节信号号到管道（async-signal-safe 白名单调用），
//     SA_RESTART 保证 reactor 的 epoll_wait/kevent 等慢系统调用不被 EINTR 打断
//     （唤醒由管道可读事件天然完成）；
//   - 读端挂 reactor 事件循环，drain 在 reactor 线程逐字节派发回调——
//     信号处理逻辑从处理器上下文迁移到事件循环线程，回调可安全跑 .lm 协程。
#include "lm_signal.h"

#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

/* 进程级单例：信号管道 + 消费登记 */
static int g_sig_pipe[2] = { -1, -1 };
static lm_signal_cb_t g_sig_cb = NULL;
static void* g_sig_cb_data = NULL;
/* 已注册信号表（sigaction 幂等与 shutdown 还原依据；信号处理器可能读，
 * sig_atomic_t 保证处理器上下文可见性） */
static volatile sig_atomic_t g_sig_watched[LM_SIGNAL_TABLE_SIZE];

/* 信号名 ↔ 信号号表（nginx signal 动作表简化版：TERM/INT/HUP） */
typedef struct {
    int signo;
    const char* name;
} lm_signal_name_t;

static const lm_signal_name_t lm_signal_names[] = {
#ifdef SIGTERM
    { SIGTERM, "TERM" },
#endif
#ifdef SIGINT
    { SIGINT,  "INT"  },
#endif
#ifdef SIGHUP
    { SIGHUP,  "HUP"  },
#endif
    { 0, NULL },
};

const char* lm_signal_name(int signo) {
    for (int i = 0; lm_signal_names[i].name; i++) {
        if (lm_signal_names[i].signo == signo) return lm_signal_names[i].name;
    }
    return NULL;
}

int lm_signal_signo(const char* name) {
    if (!name) return 0;
    for (int i = 0; lm_signal_names[i].name; i++) {
        if (strcmp(lm_signal_names[i].name, name) == 0) return lm_signal_names[i].signo;
    }
    return 0;
}

/* 复用 reactor 的 fd 属性设置惯例：非阻塞 + CLOEXEC */
static int lm_signal_fd_nonblock(int fd) {
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static int lm_signal_fd_cloexec(int fd) {
    int fl = fcntl(fd, F_GETFD, 0);
    if (fl == -1) return -1;
    return fcntl(fd, F_SETFD, fl | FD_CLOEXEC);
}

int lm_signal_init(void) {
    if (g_sig_pipe[0] >= 0) return 0;   /* 幂等 */
    int pfd[2];
    if (pipe(pfd) != 0) return -1;
    lm_signal_fd_nonblock(pfd[0]);
    lm_signal_fd_nonblock(pfd[1]);   /* 写端非阻塞：处理器内写不阻塞 */
    lm_signal_fd_cloexec(pfd[0]);
    lm_signal_fd_cloexec(pfd[1]);
    g_sig_pipe[0] = pfd[0];
    g_sig_pipe[1] = pfd[1];
    return 0;
}

/* 信号处理器：仅做 async-signal-safe 的 write（1 字节信号号）。
 * 非阻塞写：管道满（同信号字节未及消费）返回 EAGAIN，忽略即可——
 * 语义是"至少投递一次"，同信号连发合并为积压字节逐个派发是可接受语义。 */
static void lm_signal_handler(int signo) {
    int saved = errno;
    if (g_sig_pipe[1] >= 0) {
        unsigned char b = (unsigned char)signo;
        ssize_t n = write(g_sig_pipe[1], &b, 1);
        (void)n;
    }
    errno = saved;
}

int lm_signal_watch(int signo) {
    if (signo <= 0 || signo >= LM_SIGNAL_TABLE_SIZE) return -1;
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = lm_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;   /* 慢系统调用自动重启，唤醒走管道可读事件 */
    if (sigaction(signo, &sa, NULL) != 0) return -1;
    g_sig_watched[signo] = 1;
    return 0;
}

/* 信号管道读回调（reactor 线程）：drain 全部字节，逐字节派发回调。
 * 同一信号连发积压多字节时逐次回调（与 libuv 逐信号回调语义一致）。 */
static void lm_signal_drain(lm_connection_t* conn, uint32_t events, void* data) {
    (void)events; (void)data;
    unsigned char buf[64];
    while (1) {
        ssize_t n = read(conn->fd, buf, sizeof(buf));
        if (n <= 0) break;   /* EAGAIN（drain 完）或 EOF */
        for (ssize_t i = 0; i < n; i++) {
            int signo = (int)buf[i];
            if (g_sig_cb && signo > 0 && signo < LM_SIGNAL_TABLE_SIZE) {
                g_sig_cb(signo, g_sig_cb_data);
            }
        }
    }
}

int lm_signal_attach(lm_reactor_t* r, lm_signal_cb_t cb, void* data) {
    if (!r || !cb) return -1;
    if (g_sig_cb) return -1;   /* 进程内单消费者 */
    if (lm_signal_init() != 0) return -1;
    lm_connection_t* c = lm_reactor_get_connection(r, g_sig_pipe[0]);
    if (!c) return -1;   /* fd 超出连接池容量 */
    if (lm_reactor_add(r, c, LM_EVENT_READ) != 0) return -1;
    c->read_handler = lm_signal_drain;
    c->read_data = NULL;
    c->write_handler = NULL;
    c->write_data = NULL;
    c->co = NULL;
    g_sig_cb = cb;
    g_sig_cb_data = data;
    return 0;
}

void lm_signal_shutdown(void) {
    /* 恢复已注册信号为默认处理（SIG_DFL） */
    for (int s = 1; s < LM_SIGNAL_TABLE_SIZE; s++) {
        if (g_sig_watched[s]) {
            struct sigaction sa;
            memset(&sa, 0, sizeof(sa));
            sa.sa_handler = SIG_DFL;
            sigemptyset(&sa.sa_mask);
            sigaction(s, &sa, NULL);
            g_sig_watched[s] = 0;
        }
    }
    g_sig_cb = NULL;
    g_sig_cb_data = NULL;
    /* 关闭管道两端。前置约定：本调用发生在 reactor 事件循环退出后
     * （.lm 层 onStop 收尾），读端在 reactor 后端的注册随 backend_fd
     * 销毁一并失效，此处关闭不产生失效事件 */
    if (g_sig_pipe[0] >= 0) { close(g_sig_pipe[0]); g_sig_pipe[0] = -1; }
    if (g_sig_pipe[1] >= 0) { close(g_sig_pipe[1]); g_sig_pipe[1] = -1; }
}
