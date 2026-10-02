// lm_ssl.h —— TLS 传输层安全抽象（G4：TLS 服务端 + raw TLS 客户端）
//
// 命名说明：本模块的 TLS 指 Transport Layer Security；运行时里 lm_tls.*
// 已用于线程局部存储（Thread-Local Storage），故传输层安全模块取 lm_ssl。
//
// 分层（采纳成熟网络库的 SSL 抽象/后端/空实现三层结构）：
//   本头为后端无关抽象；lm_ssl.c 内按编译开关 LM_HAVE_OPENSSL 选择
//   OpenSSL 后端或 nossl 空实现（无 TLS 库时零成本构建，启用调用报双语错）。
//
// 与 socket 层的集成契约：
//   - 所有会话函数为「单步非阻塞」驱动：返回 WANT_READ/WANT_WRITE 时，
//     调用方（lm_socket.c，已在协程内）用 reactor 事件等待 helper 挂起
//     当前协程，fd 就绪后重试同一步；阻塞 fd 场景下一步到位（SSL 在内核等）。
//   - 会话对象 malloc 管理，生命周期挂在 SocketObj 上，显式 close 释放；
//     未关闭连接随进程退出回收（与 fd 同口径，GC 无终结器）。
//
// 线程安全：服务端上下文（lm_ssl_server_ctx_t）建一次后只读共享，
// 可跨 reactor/线程为每个入站连接派生会话；OpenSSL 1.1+ 全局初始化与
// SSL_CTX 引用计数本身线程安全。
#ifndef LM_SSL_H
#define LM_SSL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 单步驱动返回码 */
enum {
    LM_SSL_OK = 0,          /* 本步完成（握手完成 / 读写到数据） */
    LM_SSL_WANT_READ = 1,   /* 需要 fd 可读后重试同一步 */
    LM_SSL_WANT_WRITE = 2,  /* 需要 fd 可写后重试同一步 */
    LM_SSL_EOF = 3,         /* 对端有序关闭（close_notify / FIN） */
    LM_SSL_ERROR = -1       /* 致命错误（取 lm_ssl_last_error 文案） */
};

typedef struct lm_ssl_server_ctx lm_ssl_server_ctx_t;
typedef struct lm_ssl_session lm_ssl_session_t;

/* 构建是否启用 TLS 后端（1=可用；0=nossl 空实现，启用调用将报错） */
int lm_ssl_available(void);

/* 服务端上下文：加载证书链（PEM）+ 私钥（PEM），最低 TLS1.2。
 * 失败返回 NULL 并填 errBuf（底层英文原因，双语由调用方包装）。 */
lm_ssl_server_ctx_t* lm_ssl_server_ctx_new(const char* certPath,
                                           const char* keyPath,
                                           char* errBuf, size_t errLen);
void lm_ssl_server_ctx_free(lm_ssl_server_ctx_t* ctx);

/* 在已 accept 的裸 fd 上创建服务端 TLS 会话（不立即握手）。
 * fd 已由调用方设好阻塞/非阻塞模式；握手用 lm_ssl_handshake_step 驱动。 */
lm_ssl_session_t* lm_ssl_session_accept_new(lm_ssl_server_ctx_t* ctx, int fd);

/* 在已 connect 的裸 fd 上创建客户端 TLS 会话（不立即握手）。
 * verifyPeer=0 不校验证书（测试/内网自签场景）；=1 做默认证书链校验
 * （本期不做主机名 SAN 匹配，raw 客户端默认 0）。 */
lm_ssl_session_t* lm_ssl_session_connect_new(int fd, int verifyPeer,
                                             char* errBuf, size_t errLen);

void lm_ssl_session_free(lm_ssl_session_t* s);

/* 握手单步：服务端会话执行 SSL_accept，客户端会话执行 SSL_connect。
 * OK=握手完成；WANT_*=等事件重试；EOF/ERROR 见返回码。 */
int lm_ssl_handshake_step(lm_ssl_session_t* s);

/* 读单步：OK 且 outN>0 有数据；EOF 单独返回。数据落在 buf（容量 cap）。 */
int lm_ssl_read_step(lm_ssl_session_t* s, void* buf, size_t cap, size_t* outN);

/* 写单步：OK 且 outN>0 为本次实际写入（部分写模式，调用方按 outN 推进）。 */
int lm_ssl_write_step(lm_ssl_session_t* s, const void* buf, size_t len, size_t* outN);

/* 有序关闭（best effort：发 close_notify；调用方不关心 WANT 重试）。 */
int lm_ssl_shutdown_step(lm_ssl_session_t* s);

/* 最近一次错误的英文原因文案（OpenSSL 错误队列） */
const char* lm_ssl_last_error(lm_ssl_session_t* s);

#ifdef __cplusplus
}
#endif

#endif /* LM_SSL_H */
