// lm_net_errno.h —— 网络套接字错误码枚举
// C 层（lm_socket.c）与 lm 框架层（LumyrNetWork/NetError.lm）共用同一套数值，
// 两边逐项一对一对齐：runtime_error_code(code, "SocketError", msg) 抛出，
// lm 层 catch(e) 后 e.getCode() == NetError.XXX 精确比较（不再字符串匹配）。
// 新增网络错误码时：①在此加枚举项；②在 NetError.lm 加同值 enum 项。
#ifndef LM_NET_ERRNO_H
#define LM_NET_ERRNO_H

typedef enum {
    NET_ERR_NONE = 0,            /* 无错误（兼容 runtime_error 不带码，VAL_ERROR.code=0） */
    /* 对象/参数校验族 */
    NET_ERR_INVALID_TYPE = 1,    /* receiver 非 socket 对象 */
    NET_ERR_BAD_STATE = 2,      /* 套接字对象无效或已关闭 */
    NET_ERR_BAD_FD = 3,          /* 套接字 fd 无效 */
    NET_ERR_NO_MEMORY = 4,       /* 内存不足 */
    NET_ERR_NO_FIELD = 5,        /* 未知字段/方法名 */
    /* 地址族 */
    NET_ERR_ADDR_RESOLVE = 6,   /* 主机名/地址解析失败 */
    NET_ERR_ADDR_TIMEOUT = 7,  /* 主机名解析超时（getaddrinfo 阻塞超过限定时间） */
    /* 系统调用 errno 失败族（按操作分码，message 带 errno 文本便于诊断） */
    NET_ERR_CONNECT = 10,
    NET_ERR_BIND = 11,
    NET_ERR_LISTEN = 12,
    NET_ERR_ACCEPT = 13,
    NET_ERR_SEND = 14,
    NET_ERR_RECV = 15,
    NET_ERR_SENDTO = 16,
    NET_ERR_RECVFROM = 17,
    NET_ERR_SET_OPTION = 18,
    NET_ERR_GET_OPTION = 19,
    /* 资源耗尽族（专用码，上层按语义精确分派，如 accept 退避；不做文本匹配） */
    NET_ERR_FD_EXHAUSTED = 20,  /* fd 耗尽（EMFILE/ENFILE，系统 errno 仍随 os_errno 携带） */
    /* 超时族（专用码，上层按语义精确分派，如 HTTP 转 408） */
    NET_ERR_RECV_TIMEOUT = 30,
    NET_ERR_SEND_TIMEOUT = 31,
    /* setOption/getOption 特定错误 */
    NET_ERR_INVALID_VALUE = 40, /* 选项值类型错（需 int/bool） */
    NET_ERR_EMPTY_NAME = 41,    /* 选项名为空 */
    NET_ERR_UNSUPPORTED = 42,    /* 当前平台不支持该选项 */
    NET_ERR_UNKNOWN_OPTION = 43 /* 未知选项名 */
} NetErrorCode;

#endif /* LM_NET_ERRNO_H */
