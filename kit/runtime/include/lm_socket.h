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

// 设置 DNS 解析超时（毫秒）：build_inet/connect/bind 在 getaddrinfo 上等的最长时间。
// 0 = 不限（默认，兼容旧行为）。
// 跨平台实现用独立线程 + 条件变量超时等待，worker 线程超时后 detach 继续跑
// （资源泄漏但安全，避免 SIGALRM 中断 getaddrinfo 导致 libc 内部锁死）。
void lm_socket_set_dns_timeout_ms(int ms);

// 协程模式：设置全局 reactor 实例供 socket API 协程化 fd 事件注册。
// 协程上下文（lm_co_current() != NULL）且已 set_reactor 时，recv/send/accept/connect
// 自动走非阻塞 + yield 等事件路径（EAGAIN/EINPROGRESS 挂起协程，事件就绪后 resume 重试）；
// 否则保持原有阻塞逻辑（兼容 thread-per-conn 路径）。
// 前向声明 lm_reactor_s（完整定义在 lm_reactor.h），避免本头引入 epoll/kqueue 依赖。
struct lm_reactor_s;
void lm_socket_set_reactor(struct lm_reactor_s* r);

#endif // LM_SOCKET_H
