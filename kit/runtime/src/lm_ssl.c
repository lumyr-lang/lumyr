// lm_ssl.c —— TLS 传输层安全抽象实现（G4）
// 单文件双后端：LM_HAVE_OPENSSL 定义时为 OpenSSL 后端，否则为 nossl
// 空实现（lm_ssl_available()=0，启用调用返回明确错误，无 TLS 库的平台/
// 构建零成本降级）。
//
// 非阻塞握手/读写状态机由调用方（lm_socket.c）驱动：本层只做一次 SSL I/O
// 并把 WANT_READ/WANT_WRITE 翻译为返回码，事件等待（reactor 挂协程）不进
// 本层，保持抽象层零 reactor 依赖。
#include "lm_ssl.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef LM_HAVE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/err.h>

/* 结构定义（后端私有） */
struct lm_ssl_server_ctx {
    SSL_CTX* sslCtx;
};

struct lm_ssl_session {
    SSL* ssl;
    int isServer;
    char lastErr[256];
};

/* 把 OpenSSL 错误队列顶端原因写入 errBuf（无队列条目时给通用文案） */
static void sslDescribe(char* errBuf, size_t errLen, const char* what,
                        unsigned long code) {
    if (!errBuf || errLen == 0) return;
    char tmp[200] = {0};
    if (code == 0) code = ERR_peek_last_error();
    if (code) ERR_error_string_n(code, tmp, sizeof(tmp));
    snprintf(errBuf, errLen, "%s: %s", what, tmp[0] ? tmp : "unknown openssl error");
}

int lm_ssl_available(void) { return 1; }

lm_ssl_server_ctx_t* lm_ssl_server_ctx_new(const char* certPath,
                                           const char* keyPath,
                                           char* errBuf, size_t errLen) {
    if (!certPath || !keyPath) {
        if (errBuf && errLen) snprintf(errBuf, errLen, "cert/key path is null");
        return NULL;
    }
    SSL_CTX* c = SSL_CTX_new(TLS_server_method());
    if (!c) {
        sslDescribe(errBuf, errLen, "SSL_CTX_new failed", 0);
        return NULL;
    }
    /* 最低 TLS1.2（禁 SSLv3/TLS1.0/1.1；实际下限也受系统 OpenSSL 策略约束） */
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    if (SSL_CTX_use_certificate_chain_file(c, certPath) != 1) {
        char what[256];
        snprintf(what, sizeof(what), "load cert chain failed (%s)", certPath);
        sslDescribe(errBuf, errLen, what, 0);
        SSL_CTX_free(c);
        return NULL;
    }
    if (SSL_CTX_use_PrivateKey_file(c, keyPath, SSL_FILETYPE_PEM) != 1) {
        char what[256];
        snprintf(what, sizeof(what), "load private key failed (%s)", keyPath);
        sslDescribe(errBuf, errLen, what, 0);
        SSL_CTX_free(c);
        return NULL;
    }
    if (SSL_CTX_check_private_key(c) != 1) {
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "cert/key mismatch: private key does not match certificate");
        SSL_CTX_free(c);
        return NULL;
    }
    lm_ssl_server_ctx_t* sc = (lm_ssl_server_ctx_t*)malloc(sizeof(*sc));
    if (!sc) { SSL_CTX_free(c); return NULL; }
    sc->sslCtx = c;
    return sc;
}

void lm_ssl_server_ctx_free(lm_ssl_server_ctx_t* ctx) {
    if (!ctx) return;
    if (ctx->sslCtx) SSL_CTX_free(ctx->sslCtx);
    free(ctx);
}

/* 会话公共配置：部分写 + 可移动写缓冲——语义贴近 send(2)，
 * 调用方可按实际写入字节数推进偏移重试 */
static void sessionSetModes(SSL* ssl) {
    long mode = SSL_get_mode(ssl);
    mode |= SSL_MODE_ENABLE_PARTIAL_WRITE;
    mode |= SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER;
    SSL_set_mode(ssl, mode);
}

lm_ssl_session_t* lm_ssl_session_accept_new(lm_ssl_server_ctx_t* ctx, int fd) {
    if (!ctx || !ctx->sslCtx || fd < 0) return NULL;
    SSL* ssl = SSL_new(ctx->sslCtx);
    if (!ssl) return NULL;
    SSL_set_fd(ssl, fd);
    SSL_set_accept_state(ssl);
    sessionSetModes(ssl);
    lm_ssl_session_t* s = (lm_ssl_session_t*)calloc(1, sizeof(*s));
    if (!s) { SSL_free(ssl); return NULL; }
    s->ssl = ssl;
    s->isServer = 1;
    return s;
}

lm_ssl_session_t* lm_ssl_session_connect_new(int fd, int verifyPeer,
                                             char* errBuf, size_t errLen) {
    if (fd < 0) {
        if (errBuf && errLen) snprintf(errBuf, errLen, "invalid fd for TLS connect");
        return NULL;
    }
    SSL_CTX* c = SSL_CTX_new(TLS_client_method());
    if (!c) {
        sslDescribe(errBuf, errLen, "SSL_CTX_new failed", 0);
        return NULL;
    }
    SSL_CTX_set_min_proto_version(c, TLS1_2_VERSION);
    /* raw 客户端默认不验签（自签/内网测试）；verifyPeer 时做默认链校验。
     * 本期不做主机名 SAN 匹配（无固定 SNI 入参），仅链校验。 */
    if (!verifyPeer) SSL_CTX_set_verify(c, SSL_VERIFY_NONE, NULL);
    SSL* ssl = SSL_new(c);   /* ctx ref 1→2；本会话独占该 ctx */
    if (!ssl) {
        sslDescribe(errBuf, errLen, "SSL_new failed", 0);
        SSL_CTX_free(c);
        return NULL;
    }
    SSL_set_fd(ssl, fd);
    SSL_set_connect_state(ssl);
    sessionSetModes(ssl);
    lm_ssl_session_t* s = (lm_ssl_session_t*)calloc(1, sizeof(*s));
    if (!s) {
        if (errBuf && errLen) snprintf(errBuf, errLen, "out of memory for TLS session");
        SSL_free(ssl); SSL_CTX_free(c);
        return NULL;
    }
    s->ssl = ssl;
    s->isServer = 0;
    return s;
}

void lm_ssl_session_free(lm_ssl_session_t* s) {
    if (!s) return;
    if (s->ssl) {
        SSL_CTX* c = SSL_get_SSL_CTX(s->ssl);
        SSL_free(s->ssl);       /* ctx ref -1 */
        /* 客户端 ctx 由本会话独占（SSL_new 后 ref=2，此处 SSL_free 后 ref=1，
         * 再 free 归零）；服务端 ctx 由 lm_ssl_server_ctx 持有，绝不在此释放。 */
        if (!s->isServer && c) SSL_CTX_free(c);
    }
    free(s);
}

int lm_ssl_handshake_step(lm_ssl_session_t* s) {
    if (!s || !s->ssl) return LM_SSL_ERROR;
    int rc = s->isServer ? SSL_accept(s->ssl) : SSL_connect(s->ssl);
    if (rc == 1) return LM_SSL_OK;
    int e = SSL_get_error(s->ssl, rc);
    if (e == SSL_ERROR_WANT_READ) return LM_SSL_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE) return LM_SSL_WANT_WRITE;
    s->lastErr[0] = 0;
    if (e == SSL_ERROR_SSL || e == SSL_ERROR_SYSCALL) {
        unsigned long ec = ERR_peek_last_error();
        if (ec) ERR_error_string_n(ec, s->lastErr, sizeof(s->lastErr));
    }
    return LM_SSL_ERROR;
}

int lm_ssl_read_step(lm_ssl_session_t* s, void* buf, size_t cap, size_t* outN) {
    if (!s || !s->ssl || !buf || !outN) return LM_SSL_ERROR;
    *outN = 0;
    int n = SSL_read(s->ssl, buf, (int)cap);
    if (n > 0) { *outN = (size_t)n; return LM_SSL_OK; }
    int e = SSL_get_error(s->ssl, n);
    if (e == SSL_ERROR_WANT_READ) return LM_SSL_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE) return LM_SSL_WANT_WRITE;
    if (e == SSL_ERROR_ZERO_RETURN) return LM_SSL_EOF;  /* close_notify */
    if (e == SSL_ERROR_SYSCALL) {
        /* 无 OpenSSL 错误条目（对端直接 FIN / errno=0）：按 EOF 处理，
         * 与裸 recv 返回 0 的语义保持一致 */
        if (ERR_peek_last_error() == 0) return LM_SSL_EOF;
    }
    unsigned long ec = ERR_peek_last_error();
    if (ec) ERR_error_string_n(ec, s->lastErr, sizeof(s->lastErr));
    return LM_SSL_ERROR;
}

int lm_ssl_write_step(lm_ssl_session_t* s, const void* buf, size_t len, size_t* outN) {
    if (!s || !s->ssl || !buf || !outN) return LM_SSL_ERROR;
    *outN = 0;
    int n = SSL_write(s->ssl, buf, (int)len);
    if (n > 0) { *outN = (size_t)n; return LM_SSL_OK; }
    int e = SSL_get_error(s->ssl, n);
    if (e == SSL_ERROR_WANT_READ) return LM_SSL_WANT_READ;
    if (e == SSL_ERROR_WANT_WRITE) return LM_SSL_WANT_WRITE;
    if (e == SSL_ERROR_ZERO_RETURN) return LM_SSL_EOF;
    unsigned long ec = ERR_peek_last_error();
    if (ec) ERR_error_string_n(ec, s->lastErr, sizeof(s->lastErr));
    return LM_SSL_ERROR;
}

int lm_ssl_shutdown_step(lm_ssl_session_t* s) {
    if (!s || !s->ssl) return LM_SSL_ERROR;
    /* 单向 close_notify：不重试 WANT，未发出也随 fd close 兜底 */
    SSL_shutdown(s->ssl);
    return LM_SSL_OK;
}

const char* lm_ssl_last_error(lm_ssl_session_t* s) {
    if (!s) return "null ssl session";
    return s->lastErr[0] ? s->lastErr : "tls error (no detail)";
}

#else /* ===== nossl 空实现：无 TLS 库构建零成本降级 ===== */

int lm_ssl_available(void) { return 0; }

lm_ssl_server_ctx_t* lm_ssl_server_ctx_new(const char* certPath,
                                           const char* keyPath,
                                           char* errBuf, size_t errLen) {
    (void)certPath; (void)keyPath;
    if (errBuf && errLen)
        snprintf(errBuf, errLen, "TLS backend not enabled in this build (LM_HAVE_OPENSSL undefined)");
    return NULL;
}
void lm_ssl_server_ctx_free(lm_ssl_server_ctx_t* ctx) { (void)ctx; }
lm_ssl_session_t* lm_ssl_session_accept_new(lm_ssl_server_ctx_t* ctx, int fd) {
    (void)ctx; (void)fd; return NULL;
}
lm_ssl_session_t* lm_ssl_session_connect_new(int fd, int verifyPeer,
                                             char* errBuf, size_t errLen) {
    (void)fd; (void)verifyPeer;
    if (errBuf && errLen)
        snprintf(errBuf, errLen, "TLS backend not enabled in this build (LM_HAVE_OPENSSL undefined)");
    return NULL;
}
void lm_ssl_session_free(lm_ssl_session_t* s) { (void)s; }
int lm_ssl_handshake_step(lm_ssl_session_t* s) { (void)s; return LM_SSL_ERROR; }
int lm_ssl_read_step(lm_ssl_session_t* s, void* buf, size_t cap, size_t* outN) {
    (void)s; (void)buf; (void)cap; if (outN) *outN = 0; return LM_SSL_ERROR;
}
int lm_ssl_write_step(lm_ssl_session_t* s, const void* buf, size_t len, size_t* outN) {
    (void)s; (void)buf; (void)len; if (outN) *outN = 0; return LM_SSL_ERROR;
}
int lm_ssl_shutdown_step(lm_ssl_session_t* s) { (void)s; return LM_SSL_ERROR; }
const char* lm_ssl_last_error(lm_ssl_session_t* s) {
    (void)s; return "TLS backend not enabled";
}

#endif /* LM_HAVE_OPENSSL */
