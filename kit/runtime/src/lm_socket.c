// lm_socket.c —— 网络套接字对象（VAL_SOCKET）
// 统一封装 TCP / UDP / Unix 域流 / Unix 域数据报 套接字
// 持有 fd 描述符；close() 关闭并置 closed=1，GC 回收时兜底关闭
#include "lm_socket.h"
#include "lm_array.h"
#include "lm_string.h"
#include "lm_container.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

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

// 报错包装：拼 errno 文本并 runtime_error
static void sock_error(const char* op) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s() 失败: %s", op, strerror(errno));
    runtime_error(buf);
}

// 解析 host+port → sockaddr_in（IPv4），返回 0 成功
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
    // 走 getaddrinfo 解析主机名
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = 0;
    if(getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        if(res) freeaddrinfo(res);
        return -1;
    }
    memcpy(addr, res->ai_addr, sizeof(struct sockaddr_in));
    addr->sin_port = htons((uint16_t)port);
    freeaddrinfo(res);
    return 0;
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
    return lumyr_make_int(0);
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
    if(v.type != VAL_SOCKET) { runtime_error("connect() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error("connect() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error("connect() 套接字 fd 无效"); return val_none(); }
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        if(connect(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error("connect");
            return val_none();
        }
    } else {
        struct sockaddr_in addr;
        if(build_inet(host ? host : "127.0.0.1", port, &addr) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "connect() 无法解析地址: %s", host ? host : "");
            runtime_error(buf);
            return val_none();
        }
        if(connect(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error("connect");
            return val_none();
        }
    }
    o->is_connected = 1;
    return val_none();
}

Value lumyr_socket_bind(Value v, const char* host, int port) {
    if(v.type != VAL_SOCKET) { runtime_error("bind() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error("bind() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error("bind() 套接字 fd 无效"); return val_none(); }
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        /* Unix 域路径已存在则先清理，避免 EADDRINUSE */
        unlink(addr.sun_path);
        if(bind(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error("bind");
            return val_none();
        }
    } else {
        struct sockaddr_in addr;
        if(build_inet(host ? host : "0.0.0.0", port, &addr) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "bind() 无法解析地址: %s", host ? host : "");
            runtime_error(buf);
            return val_none();
        }
        if(bind(o->fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            sock_error("bind");
            return val_none();
        }
    }
    /* bind 即服务端绑定（TCP 随后 listen 会再置位；UDP/Unix 数据报 bind 即可接收） */
    o->is_server = 1;
    return val_none();
}

Value lumyr_socket_listen(Value v, int backlog) {
    if(v.type != VAL_SOCKET) { runtime_error("listen() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error("listen() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error("listen() 套接字 fd 无效"); return val_none(); }
    if(backlog <= 0) backlog = 128;
    if(listen(o->fd, backlog) != 0) {
        sock_error("listen");
        return val_none();
    }
    o->is_server = 1;
    return val_none();
}

Value lumyr_socket_accept(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error("accept() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed) { runtime_error("accept() 套接字对象无效或已关闭"); return val_none(); }
    if(o->fd < 0) { runtime_error("accept() 套接字 fd 无效"); return val_none(); }
    struct sockaddr_storage cli;
    socklen_t clilen = sizeof(cli);
    int cfd = accept(o->fd, (struct sockaddr*)&cli, &clilen);
    if(cfd < 0) {
        sock_error("accept");
        return val_none();
    }
    /* 新客户端套接字，类型与服务端一致，已连接 */
    return lumyr_socket_from_fd(cfd, (int)o->kind, 1);
}

Value lumyr_socket_send(Value v, const char* data, int len, int flags) {
    if(v.type != VAL_SOCKET) { runtime_error("send() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("send() 套接字无效或已关闭"); return lumyr_make_int(-1); }
    if(!data) data = "";
    if(len < 0) len = (int)strlen(data);
    ssize_t n = send(o->fd, data, (size_t)len, flags);
    if(n < 0) {
        sock_error("send");
        return lumyr_make_int(-1);
    }
    return lumyr_make_int((int)n);
}

Value lumyr_socket_recv(Value v, int maxLen, int flags, int asBytes) {
    if(v.type != VAL_SOCKET) { runtime_error("recv() 仅适用于 socket 对象"); return lumyr_make_string(""); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("recv() 套接字无效或已关闭"); return lumyr_make_string(""); }
    if(maxLen <= 0) maxLen = 4096;
    char* buf = (char*)malloc((size_t)maxLen + 1);
    if(!buf) { runtime_error("recv() 内存不足"); return lumyr_make_string(""); }
    ssize_t n = recv(o->fd, buf, (size_t)maxLen, flags);
    if(n < 0) {
        free(buf);
        sock_error("recv");
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
    if(v.type != VAL_SOCKET) { runtime_error("sendTo() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("sendTo() 套接字无效或已关闭"); return lumyr_make_int(-1); }
    if(!data) data = "";
    if(len < 0) len = (int)strlen(data);
    if(kind_is_unix(o->kind)) {
        struct sockaddr_un addr;
        build_unix(host ? host : "", &addr);
        ssize_t n = sendto(o->fd, data, (size_t)len, flags,
                           (struct sockaddr*)&addr, sizeof(addr));
        if(n < 0) { sock_error("sendTo"); return lumyr_make_int(-1); }
        return lumyr_make_int((int)n);
    } else {
        struct sockaddr_in addr;
        if(build_inet(host ? host : "127.0.0.1", port, &addr) != 0) {
            char buf[256];
            snprintf(buf, sizeof(buf), "sendTo() 无法解析地址: %s", host ? host : "");
            runtime_error(buf);
            return lumyr_make_int(-1);
        }
        ssize_t n = sendto(o->fd, data, (size_t)len, flags,
                           (struct sockaddr*)&addr, sizeof(addr));
        if(n < 0) { sock_error("sendTo"); return lumyr_make_int(-1); }
        return lumyr_make_int((int)n);
    }
}

Value lumyr_socket_recvfrom(Value v, int maxLen, int flags) {
    if(v.type != VAL_SOCKET) { runtime_error("recvFrom() 仅适用于 socket 对象"); return val_array(0); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("recvFrom() 套接字无效或已关闭"); return val_array(0); }
    if(maxLen <= 0) maxLen = 4096;
    char* buf = (char*)malloc((size_t)maxLen + 1);
    if(!buf) { runtime_error("recvFrom() 内存不足"); return val_array(0); }
    struct sockaddr_storage src;
    socklen_t srclen = sizeof(src);
    ssize_t n = recvfrom(o->fd, buf, (size_t)maxLen, flags,
                         (struct sockaddr*)&src, &srclen);
    if(n < 0) {
        free(buf);
        sock_error("recvFrom");
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
    if(v.type != VAL_SOCKET) { runtime_error("close() 仅适用于 socket 对象"); return val_none(); }
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
    if(v.type != VAL_SOCKET) { runtime_error("setOption() 仅适用于 socket 对象"); return val_none(); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("setOption() 套接字无效或已关闭"); return val_none(); }
    if(!name) { runtime_error("setOption(name, val) 选项名为空"); return val_none(); }
    /* 通用 int 值提取（bool/int 均按整数处理） */
    int ival = 0;
    if(val.type == VAL_BOOL) ival = val.v.b ? 1 : 0;
    else if(val.type == VAL_INT) ival = val.v.i;
    else if(val.type == VAL_INT64) ival = (int)val.v.i64;
    else if(val.type == VAL_DOUBLE) ival = (int)val.v.d;
    else { runtime_error("setOption() 值需为 int/bool"); return val_none(); }

    if(strcmp(name, "nonBlock") == 0) {
        int flags = fcntl(o->fd, F_GETFL, 0);
        if(ival) flags |= O_NONBLOCK; else flags &= ~O_NONBLOCK;
        if(fcntl(o->fd, F_SETFL, flags) != 0) { sock_error("setOption(nonBlock)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "reuseAddr") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) != 0) { sock_error("setOption(reuseAddr)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "reusePort") == 0) {
#ifdef SO_REUSEPORT
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) != 0) { sock_error("setOption(reusePort)"); return val_none(); }
        return val_none();
#else
        runtime_error("setOption(reusePort) 当前平台不支持");
        return val_none();
#endif
    }
    if(strcmp(name, "broadcast") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on)) != 0) { sock_error("setOption(broadcast)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "keepAlive") == 0) {
        int on = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on)) != 0) { sock_error("setOption(keepAlive)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "sndBuf") == 0) {
        int sz = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz)) != 0) { sock_error("setOption(sndBuf)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "rcvBuf") == 0) {
        int sz = ival;
        if(setsockopt(o->fd, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz)) != 0) { sock_error("setOption(rcvBuf)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "recvTimeout") == 0) {
        struct timeval tv;
        tv.tv_sec = ival / 1000;
        tv.tv_usec = (ival % 1000) * 1000;
        if(setsockopt(o->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) { sock_error("setOption(recvTimeout)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "sendTimeout") == 0) {
        struct timeval tv;
        tv.tv_sec = ival / 1000;
        tv.tv_usec = (ival % 1000) * 1000;
        if(setsockopt(o->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) != 0) { sock_error("setOption(sendTimeout)"); return val_none(); }
        return val_none();
    }
    if(strcmp(name, "tcpNoDelay") == 0) {
        int on = ival;
        if(setsockopt(o->fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on)) != 0) { sock_error("setOption(tcpNoDelay)"); return val_none(); }
        return val_none();
    }
    char buf[256];
    snprintf(buf, sizeof(buf), "setOption() 未知选项: %s", name);
    runtime_error(buf);
    return val_none();
}

Value lumyr_socket_get_option(Value v, const char* name) {
    if(v.type != VAL_SOCKET) { runtime_error("getOption() 仅适用于 socket 对象"); return lumyr_make_int(0); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o || o->closed || o->fd < 0) { runtime_error("getOption() 套接字无效或已关闭"); return lumyr_make_int(0); }
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
    runtime_error(buf);
    return lumyr_make_int(0);
}

Value lumyr_socket_fileno(Value v) {
    if(v.type != VAL_SOCKET) { runtime_error("fileno() 仅适用于 socket 对象"); return lumyr_make_int(-1); }
    SocketObj* o = (SocketObj*)v.v.socket_obj;
    if(!o) return lumyr_make_int(-1);
    return lumyr_make_int(o->closed ? -1 : o->fd);
}
