// lm_ws.h —— WebSocket（RFC 6455）帧协议：解析状态机 + 帧编码 + 握手密钥
// 按 RFC 6455 公开规格自写实现（净室）：帧格式/掩码/opcode 均为协议标准。
//
// 分层：HTTP 层完成 101 升级后，连接进入帧消息循环；字节流交给
// 本层解析器（feed 模式，可跨 recv 边界粘包/拆包），产出完整帧事件；
// 发送侧用 lm_ws_frame_encode 组帧。
//
// 句柄为进程内 int 编号（句柄表，非裸指针）；同一解析器句柄的调用
// 须来自同一线程/协程上下文（流状态不可交错），不同句柄间并发安全。
#ifndef LM_WS_H
#define LM_WS_H

#include <stdint.h>
#include <stddef.h>

// opcode（RFC 6455 §5.2）
#define LM_WS_OP_CONT   0x0   // 续帧
#define LM_WS_OP_TEXT   0x1   // 文本
#define LM_WS_OP_BIN    0x2   // 二进制
#define LM_WS_OP_CLOSE  0x8   // 关闭
#define LM_WS_OP_PING   0x9   // ping
#define LM_WS_OP_PONG   0xA   // pong

// 单帧负载上限（防恶意长度字段耗尽内存；消息级上限由 .lm 层重组时控制）
#define LM_WS_MAX_FRAME_PAYLOAD (64u * 1024u * 1024u)

// 解析事件：一条完整帧（分片帧逐帧产出，重组在 .lm 层）
typedef struct {
    int      fin;        // 末帧标记
    int      opcode;     // LM_WS_OP_*
    uint8_t* payload;    // malloc（len>0 时），调用方 free
    uint64_t len;        // payload 长度
} LmWsEvent;

// Sec-WebSocket-Accept 计算：Base64(SHA1(key + RFC6455 固定 GUID))。
// 返回 malloc 字符串（调用方 free）；失败返回 NULL。
char* lm_ws_accept_key(const char* key);

// 帧编码：fin/opcode/mask + payload → 完整帧字节（malloc，调用方 free）。
//   mask 非 0 时生成随机掩码并按 RFC 掩码负载（客户端角色用；
//   服务端发送必须 mask=0）
// 成功返回 0，*out/*outLen 产出；失败返回 -1。
int lm_ws_frame_encode(int fin, int opcode, int mask,
                       const uint8_t* payload, uint64_t len,
                       uint8_t** out, size_t* outLen);

// 解析器句柄生命周期
//   expectMasked 非 0：要求入向帧必须带掩码（服务端角色，RFC 强制）；
//   0：要求不带掩码（客户端角色）
int  lm_ws_parser_create(int expectMasked);
void lm_ws_parser_destroy(int h);

// 喂入字节流，产出 0..n 条完整帧事件。
//   成功返回 0：*events 为 malloc 数组（调用方逐条 free payload 后 free 数组），
//   *count 为条数（可为 0——数据不足一帧时缓存于解析器内）。
//   失败返回 -1：协议错误（errBuf 双语描述），连接应按 1002 关闭；
//   此后解析器状态不可再用，调用方应 destroy。
int lm_ws_parser_feed(int h, const uint8_t* data, size_t len,
                      LmWsEvent** events, int* count,
                      char* errBuf, size_t errLen);

#endif // LM_WS_H
