// lm_socket.h —— 网络套接字对象（VAL_SOCKET）
// 统一封装 TCP / UDP / Unix 域流 / Unix 域数据报 套接字
// 持有 fd 描述符；close() 关闭并置 closed=1，GC 回收时兜底关闭
#ifndef LM_SOCKET_H
#define LM_SOCKET_H

#include "lm_value.h"
#include "lumyr_value_type.h"

// ===== 构造 =====
// 按类型构造未连接套接字：tcpSocket() / udpSocket() / unixSocket() / unixDgramSocket()
Value lumyr_socket_make(int kind);
// 从已存在 fd 构造（accept 返回的客户端套接字）
Value lumyr_socket_from_fd(int fd, int kind, int connected);

// 字段访问（统一入口）：fd / kind / closed / connected / isServer
Value lumyr_socket_field(Value v, const char* name);

// ISO/字符串化（malloc，调用方 free）
char* lumyr_socket_to_str(Value v);

// ===== 方法 =====
// connect：TCP/UDP 用 host+port；Unix 流/数据报用 path（port 忽略）
Value lumyr_socket_connect(Value v, const char* host, int port);
// bind：TCP/UDP 用 host+port；Unix 用 path（port 忽略，先 unlink 旧路径）
Value lumyr_socket_bind(Value v, const char* host, int port);
// listen：TCP/Unix 流服务端监听
Value lumyr_socket_listen(Value v, int backlog);
// accept：TCP/Unix 流服务端接受连接 → 新 socket（阻塞）；失败返回 null
Value lumyr_socket_accept(Value v);
// send：已连接套接字发送（TCP/Unix 流，或 UDP 已 connect）→ 实际发送字节数
Value lumyr_socket_send(Value v, const char* data, int len, int flags);
// recv：已连接套接字接收 → 字符串（asBytes=0）或 bytes（asBytes=1，二进制安全含 NUL）
// EOF/关闭返回 ""（字符串）或空 bytes
Value lumyr_socket_recv(Value v, int maxLen, int flags, int asBytes);
// sendTo：UDP/Unix 数据报指定目标发送 → 实际发送字节数
Value lumyr_socket_sendto(Value v, const char* data, int len,
                          const char* host, int port, int flags);
// recvFrom：UDP/Unix 数据报接收 → [data, addrInfo]（addrInfo="host:port" 或 path）
Value lumyr_socket_recvfrom(Value v, int maxLen, int flags);
// close：关闭套接字
Value lumyr_socket_close(Value v);
// setOption：设置套接字选项（reuseAddr/reusePort/broadcast/sndBuf/rcvBuf/nonBlock/recvTimeout/sendTimeout）
Value lumyr_socket_set_option(Value v, const char* name, Value val);
// getOption：读取套接字选项
Value lumyr_socket_get_option(Value v, const char* name);
// fileno：返回 fd 描述符
Value lumyr_socket_fileno(Value v);

#endif // LM_SOCKET_H
