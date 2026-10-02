// lm_gzip.h —— gzip（RFC 1952）压缩/解压抽象（G6）
// 后端无关接口：LM_HAVE_ZLIB 定义时为系统 zlib 实现（动态链接 -lz，
// zlib License，许可文本见 licenses/zlib-LICENSE），否则为 nozlib
// 空实现（lm_gzip_available()=0，调用返回明确错误，无 zlib 平台零成本降级）。
//
// 本层只做一次性缓冲进/缓冲出（buffer-in/buffer-out）：HTTP 响应体在
// 过滤器中已完整可得，单次 deflate/inflate 最简且无流式状态外泄。
// 超大响应的流式压缩（配合 chunked 逐块）留待后续 G7 同期再评估。
#ifndef LM_GZIP_H
#define LM_GZIP_H

#include <stdint.h>
#include <stddef.h>

// 当前构建是否启用 zlib 后端（.lm 层可据此降级/报错）
int lm_gzip_available(void);

// gzip 压缩（gzip 流封装：deflate windowBits=15+16，非裸 zlib 封装）。
//   in/inLen 原始数据（二进制安全，可含 NUL）
//   level    -1=zlib 默认(6)，0=仅存储，1..9 速度↔压缩率
// 成功返回 0，*out 指向 malloc 的 gzip 数据（调用方 free），*outLen 为长度；
// 失败返回 -1，errBuf（可空）写入双语可读原因。
int lm_gzip_compress(const uint8_t* in, size_t inLen, int level,
                     uint8_t** out, size_t* outLen,
                     char* errBuf, size_t errLen);

// gzip 解压（自动识别 gzip 封装）。成功返回 0，*out/*outLen 语义同上。
int lm_gzip_decompress(const uint8_t* in, size_t inLen,
                       uint8_t** out, size_t* outLen,
                       char* errBuf, size_t errLen);

// ===== 流式增量压缩（有状态 z_stream 句柄，G7 与 WebSocket 同期） =====
// 用途：chunked 流式响应逐块压缩（GzipFilter 对 streamBody 的包装）。
// 句柄为进程内 int 编号（句柄表，非指针）；同一句柄的调用须来自同一
// 线程/协程上下文（压缩上下文不可交错），不同句柄间并发安全。

// 创建压缩句柄（deflateInit2，gzip 封装）。成功返回句柄(>=0)，失败 -1。
int lm_gzip_stream_create(int level, char* errBuf, size_t errLen);

// 写入一块明文并取回本轮产出的 gzip 字节（malloc，调用方 free；可为 0 长）。
//   flush 非 0：Z_SYNC_FLUSH——块边界对齐刷新，对端可立即解压到本块
//              （chunked 逐块可见性所需，开销为每块几字节同步标记）
// 失败返回 -1（句柄仍存活，可继续或 finish 销毁）。
int lm_gzip_stream_write(int h, const uint8_t* in, size_t inLen, int flush,
                         uint8_t** out, size_t* outLen,
                         char* errBuf, size_t errLen);

// 收尾：deflate(Z_FINISH) 取回尾部字节并销毁句柄（任何路径都销毁，
// 失败时也返回 -1 且句柄已释放）。成功返回 0。
int lm_gzip_stream_finish(int h, uint8_t** out, size_t* outLen,
                          char* errBuf, size_t errLen);

#endif // LM_GZIP_H
