// lm_gzip.c —— gzip 压缩/解压实现（G6）
// 单文件双后端：LM_HAVE_ZLIB 定义时接系统 zlib（动态链接，许可见
// licenses/zlib-LICENSE），否则为 nozlib 空实现。所有封装代码为本项目
// 自写，仅调用 zlib 公共 API（deflateInit2/inflateInit2 等），未复制
// 任何第三方源码。
#include "lm_gzip.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#ifdef LM_HAVE_ZLIB
#include <zlib.h>

/* gzip 解压器初始输出缓冲倍数（输入的 2 倍，最小 64 字节），
 * 不足时倍增重试探顶；zlib 无解压上界查询，只能增长式探测 */
#define LM_GZIP_INIT_OUT_MULT 2
#define LM_GZIP_INIT_OUT_MIN  64

/* 把 zlib 返回码/流内消息拼进 errBuf（msg 可为 NULL） */
static void gzipDescribe(char* errBuf, size_t errLen, const char* what,
                         int rc, const char* msg) {
    if (!errBuf || errLen == 0) return;
    snprintf(errBuf, errLen, "%s: zlib rc=%d%s%s",
             what, rc, msg ? " (" : "", msg ? msg : "");
    if (msg) {
        size_t cur = strnlen(errBuf, errLen);
        if (cur + 2 <= errLen) { errBuf[cur] = ')'; errBuf[cur + 1] = 0; }
    }
}

int lm_gzip_available(void) { return 1; }

int lm_gzip_compress(const uint8_t* in, size_t inLen, int level,
                     uint8_t** out, size_t* outLen,
                     char* errBuf, size_t errLen) {
    if (!out || !outLen) return -1;
    *out = NULL; *outLen = 0;
    if (!in && inLen > 0) {
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 压缩输入为空 / gzip compress got null input");
        return -1;
    }
    /* 级别规整：-1=默认，0..9 合法；其余值拒绝（不静默篡改用户配置） */
    if (level != -1 && (level < 0 || level > 9)) {
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 压缩级别必须为 -1 或 0..9 / gzip level must be -1 or 0..9");
        return -1;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* MAX_WBITS+16：要求 gzip 流封装（RFC 1952 头尾+CRC32），
     * memLevel=8 为 zlib 推荐默认，策略 DEFAULT（文本/二进制通用） */
    int rc = deflateInit2(&strm, level, Z_DEFLATED, MAX_WBITS + 16,
                          8, Z_DEFAULT_STRATEGY);
    if (rc != Z_OK) {
        gzipDescribe(errBuf, errLen, "gzip 初始化失败 / deflateInit2 failed",
                     rc, strm.msg);
        return -1;
    }

    uLong bound = deflateBound(&strm, (uLong)inLen);
    uint8_t* dst = (uint8_t*)malloc(bound);
    if (!dst) {
        deflateEnd(&strm);
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 输出缓冲内存不足 / gzip out of memory");
        return -1;
    }
    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)inLen;
    strm.next_out = dst;
    strm.avail_out = (uInt)bound;

    rc = deflate(&strm, Z_FINISH);
    if (rc != Z_STREAM_END) {
        gzipDescribe(errBuf, errLen, "gzip 压缩失败 / deflate failed", rc, strm.msg);
        deflateEnd(&strm);
        free(dst);
        return -1;
    }
    *outLen = (size_t)strm.total_out;
    deflateEnd(&strm);
    *out = dst;   /* 缓冲按 bound 申请，尾部余量无害（调用方按 outLen 使用） */
    return 0;
}

int lm_gzip_decompress(const uint8_t* in, size_t inLen,
                       uint8_t** out, size_t* outLen,
                       char* errBuf, size_t errLen) {
    if (!out || !outLen) return -1;
    *out = NULL; *outLen = 0;
    if (!in || inLen == 0) {
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 解压输入为空 / gzip decompress got empty input");
        return -1;
    }

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    /* MAX_WBITS+16：仅接受 gzip 封装；ENABLE_ZIP 自动识别位（32）会放宽到
     * 裸 zlib 封装，本接口语义限定 gzip，故不启用自动识别 */
    int rc = inflateInit2(&strm, MAX_WBITS + 16);
    if (rc != Z_OK) {
        gzipDescribe(errBuf, errLen, "gzip 解压初始化失败 / inflateInit2 failed",
                     rc, strm.msg);
        return -1;
    }

    size_t cap = inLen * LM_GZIP_INIT_OUT_MULT;
    if (cap < LM_GZIP_INIT_OUT_MIN) cap = LM_GZIP_INIT_OUT_MIN;
    uint8_t* dst = (uint8_t*)malloc(cap);
    if (!dst) {
        inflateEnd(&strm);
        if (errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 输出缓冲内存不足 / gzip out of memory");
        return -1;
    }

    strm.next_in = (Bytef*)in;
    strm.avail_in = (uInt)inLen;
    size_t total = 0;
    for (;;) {
        strm.next_out = dst + total;
        strm.avail_out = (uInt)(cap - total);
        rc = inflate(&strm, Z_NO_FLUSH);
        total = (size_t)strm.total_out;
        if (rc == Z_STREAM_END) break;
        if (rc == Z_OK || rc == Z_BUF_ERROR) {
            /* 输出空间耗尽 → 倍增缓冲后继续；Z_BUF_NO_PROGRESS 且无输入进展
             * 时（损坏流伪装）倍增也会因 avail_in 不动而最终触达下面的
             * avail_in 检查报错，不会死循环 */
            if (strm.avail_out != 0) {
                gzipDescribe(errBuf, errLen,
                             "gzip 数据损坏，解压提前停滞 / gzip stream corrupt, stalled",
                             rc, strm.msg);
                inflateEnd(&strm); free(dst);
                return -1;
            }
            size_t newCap = cap * 2;
            if (newCap <= cap) {   /* size_t 溢出防护 */
                gzipDescribe(errBuf, errLen,
                             "gzip 解压结果过大 / gzip output too large", rc, strm.msg);
                inflateEnd(&strm); free(dst);
                return -1;
            }
            uint8_t* grown = (uint8_t*)realloc(dst, newCap);
            if (!grown) {
                inflateEnd(&strm); free(dst);
                if (errBuf && errLen)
                    snprintf(errBuf, errLen, "gzip 输出缓冲内存不足 / gzip out of memory");
                return -1;
            }
            dst = grown;
            cap = newCap;
            continue;
        }
        /* Z_DATA_ERROR（非 gzip 流/CRC 错）等：损坏或非 gzip 数据 */
        gzipDescribe(errBuf, errLen,
                     "gzip 数据损坏或格式错误 / invalid or corrupt gzip data",
                     rc, strm.msg);
        inflateEnd(&strm);
        free(dst);
        return -1;
    }
    inflateEnd(&strm);
    *out = dst;
    *outLen = total;
    return 0;
}

/* ===== 流式增量压缩：z_stream 句柄表 =====
 * 句柄为表槽位下标 + 代数校验（槽复用时旧句柄自然失效）。
 * 表操作全程持锁；单句柄的 deflate 调用不持锁（约定同句柄不并发）。 */
#define LM_GZIP_MAX_STREAMS 1024

typedef struct {
    z_stream strm;
    uint32_t gen;      /* 代数：create 时递增，校验防野句柄 */
    int inUse;
} LmGzipSlot;

static LmGzipSlot g_gzSlots[LM_GZIP_MAX_STREAMS];
static pthread_mutex_t g_gzLock = PTHREAD_MUTEX_INITIALIZER;

/* 句柄编码：slot * 4096 + (gen % 4096)（gen 永不为 0，0 保留为空槽标记） */
static int gzMakeHandle(int slot, uint32_t gen) { return slot * 4096 + (int)(gen % 4096); }

static LmGzipSlot* gzLookup(int h) {
    if(h < 0) return NULL;
    int slot = h / 4096;
    uint32_t gen = (uint32_t)(h % 4096);
    if(slot >= LM_GZIP_MAX_STREAMS) return NULL;
    LmGzipSlot* s = &g_gzSlots[slot];
    if(!s->inUse || (s->gen % 4096) != gen) return NULL;
    return s;
}

int lm_gzip_stream_create(int level, char* errBuf, size_t errLen) {
    if(level != -1 && (level < 0 || level > 9)) {
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 压缩级别必须为 -1 或 0..9 / gzip level must be -1 or 0..9");
        return -1;
    }
    pthread_mutex_lock(&g_gzLock);
    int slot = -1;
    for(int i = 0; i < LM_GZIP_MAX_STREAMS; i++) {
        if(!g_gzSlots[i].inUse) { slot = i; break; }
    }
    if(slot < 0) {
        pthread_mutex_unlock(&g_gzLock);
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 流句柄数超限 / gzip stream handle table full");
        return -1;
    }
    LmGzipSlot* s = &g_gzSlots[slot];
    memset(&s->strm, 0, sizeof(s->strm));
    int rc = deflateInit2(&s->strm, level, Z_DEFLATED, MAX_WBITS + 16,
                          8, Z_DEFAULT_STRATEGY);
    if(rc != Z_OK) {
        pthread_mutex_unlock(&g_gzLock);
        gzipDescribe(errBuf, errLen, "gzip 流初始化失败 / deflateInit2 failed", rc, NULL);
        return -1;
    }
    s->gen++;
    if(s->gen % 4096 == 0) s->gen++;   /* 跳过 0（空槽标记） */
    s->inUse = 1;
    int h = gzMakeHandle(slot, s->gen);
    pthread_mutex_unlock(&g_gzLock);
    return h;
}

int lm_gzip_stream_write(int h, const uint8_t* in, size_t inLen, int flush,
                         uint8_t** out, size_t* outLen,
                         char* errBuf, size_t errLen) {
    if(!out || !outLen) return -1;
    *out = NULL; *outLen = 0;
    LmGzipSlot* s = gzLookup(h);
    if(!s) {
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 流句柄无效 / invalid gzip stream handle");
        return -1;
    }
    /* 输出上界：deflateBound 覆盖本块 + 存量；SYNC_FLUSH 再加 16 字节余量 */
    uLong bound = deflateBound(&s->strm, (uLong)inLen) + 16;
    uint8_t* dst = (uint8_t*)malloc(bound);
    if(!dst) {
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 输出缓冲内存不足 / gzip out of memory");
        return -1;
    }
    s->strm.next_in = (Bytef*)in;
    s->strm.avail_in = (uInt)inLen;
    s->strm.next_out = dst;
    s->strm.avail_out = (uInt)bound;
    int rc = deflate(&s->strm, flush ? Z_SYNC_FLUSH : Z_NO_FLUSH);
    if(rc != Z_OK && rc != Z_BUF_ERROR) {
        gzipDescribe(errBuf, errLen, "gzip 流压缩失败 / deflate failed", rc, s->strm.msg);
        free(dst);
        return -1;
    }
    *outLen = (size_t)((uint8_t*)s->strm.next_out - dst);
    *out = dst;
    return 0;
}

int lm_gzip_stream_finish(int h, uint8_t** out, size_t* outLen,
                          char* errBuf, size_t errLen) {
    if(!out || !outLen) return -1;
    *out = NULL; *outLen = 0;
    pthread_mutex_lock(&g_gzLock);
    LmGzipSlot* s = gzLookup(h);
    if(!s) {
        pthread_mutex_unlock(&g_gzLock);
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 流句柄无效 / invalid gzip stream handle");
        return -1;
    }
    /* 先占位再解锁：销毁语义下不再接受其他线程拿到本句柄 */
    s->inUse = 0;
    pthread_mutex_unlock(&g_gzLock);

    uLong bound = deflateBound(&s->strm, 0) + 32;
    uint8_t* dst = (uint8_t*)malloc(bound);
    if(!dst) {
        deflateEnd(&s->strm);
        if(errBuf && errLen)
            snprintf(errBuf, errLen, "gzip 输出缓冲内存不足 / gzip out of memory");
        return -1;
    }
    s->strm.next_in = NULL;
    s->strm.avail_in = 0;
    s->strm.next_out = dst;
    s->strm.avail_out = (uInt)bound;
    int rc = deflate(&s->strm, Z_FINISH);
    if(rc != Z_STREAM_END) {
        gzipDescribe(errBuf, errLen, "gzip 流收尾失败 / deflate finish failed", rc, s->strm.msg);
        deflateEnd(&s->strm);
        free(dst);
        return -1;
    }
    *outLen = (size_t)((uint8_t*)s->strm.next_out - dst);
    deflateEnd(&s->strm);
    *out = dst;
    return 0;
}

#else /* ===== nozlib 空实现：无 zlib 构建零成本降级 ===== */

int lm_gzip_available(void) { return 0; }

static void gzipNoBackend(char* errBuf, size_t errLen) {
    if (errBuf && errLen)
        snprintf(errBuf, errLen,
                 "当前构建未启用 gzip（需 zlib 开发库重新构建）/ gzip(zlib) backend not enabled in this build");
}

int lm_gzip_compress(const uint8_t* in, size_t inLen, int level,
                     uint8_t** out, size_t* outLen,
                     char* errBuf, size_t errLen) {
    (void)in; (void)inLen; (void)level; (void)out; (void)outLen;
    gzipNoBackend(errBuf, errLen);
    return -1;
}

int lm_gzip_decompress(const uint8_t* in, size_t inLen,
                       uint8_t** out, size_t* outLen,
                       char* errBuf, size_t errLen) {
    (void)in; (void)inLen; (void)out; (void)outLen;
    gzipNoBackend(errBuf, errLen);
    return -1;
}

int lm_gzip_stream_create(int level, char* errBuf, size_t errLen) {
    (void)level;
    gzipNoBackend(errBuf, errLen);
    return -1;
}

int lm_gzip_stream_write(int h, const uint8_t* in, size_t inLen, int flush,
                         uint8_t** out, size_t* outLen,
                         char* errBuf, size_t errLen) {
    (void)h; (void)in; (void)inLen; (void)flush; (void)out; (void)outLen;
    gzipNoBackend(errBuf, errLen);
    return -1;
}

int lm_gzip_stream_finish(int h, uint8_t** out, size_t* outLen,
                          char* errBuf, size_t errLen) {
    (void)h; (void)out; (void)outLen;
    gzipNoBackend(errBuf, errLen);
    return -1;
}

#endif /* LM_HAVE_ZLIB */
