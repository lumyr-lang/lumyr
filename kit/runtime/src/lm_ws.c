// lm_ws.c —— WebSocket（RFC 6455）帧协议实现（G7）
// 按 RFC 6455 公开规格净室自写：帧头布局、掩码算法、长度扩展（126/127）、
// 控制帧规则、分片规则均为协议标准本身（协议不受版权保护）。
//
// 解析器为逐字节状态机（feed 模式）：输入可任意粘包/拆包，输出完整帧事件。
// 协议校验（RFC 6455 §5）：
//   - RSV1..3 必须为 0（未协商扩展）
//   - 服务端角色要求入向帧带掩码（expectMasked=1），客户端角色反之
//   - 控制帧（opcode >= 0x8）：FIN=1 且负载 <= 125
//   - 续帧只能在未完成分片消息内；数据帧不能在分片消息进行中开启
//   - 长度取最小编码（<=125 不得用 126，<=65535 不得用 127）
//   - 127 扩展长度最高位必须为 0（RFC 规定 2^63 上限）
#include "lm_ws.h"
#include "lm_crypto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>

// RFC 6455 §4.2.2 握手 GUID
#define LM_WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

char* lm_ws_accept_key(const char* key) {
    if(!key) return NULL;
    size_t keyLen = strlen(key);
    size_t bufLen = keyLen + sizeof(LM_WS_GUID) - 1;
    char* buf = (char*)malloc(bufLen);
    if(!buf) return NULL;
    memcpy(buf, key, keyLen);
    memcpy(buf + keyLen, LM_WS_GUID, sizeof(LM_WS_GUID) - 1);
    uint8_t digest[20];
    lumyr_sha1((const uint8_t*)buf, bufLen, digest);
    free(buf);
    return lumyr_base64_encode((const char*)digest, 20);
}

// ===== 帧编码 =====

/* 掩码密钥：无需密码学强度（RFC 只要求不可预测性防中间盒缓存投毒），
 * 平台有快速随机源则用，否则退回时间+计数混合 */
static void wsMaskKey(uint8_t key[4]) {
#if defined(__APPLE__)
    arc4random_buf(key, 4);
#else
    static uint32_t s_ctr = 0;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t v = (uint32_t)ts.tv_nsec ^ ((uint32_t)ts.tv_sec << 16) ^ (++s_ctr * 2654435761u);
    key[0] = (uint8_t)(v & 0xFF);
    key[1] = (uint8_t)((v >> 8) & 0xFF);
    key[2] = (uint8_t)((v >> 16) & 0xFF);
    key[3] = (uint8_t)((v >> 24) & 0xFF);
#endif
}

int lm_ws_frame_encode(int fin, int opcode, int mask,
                       const uint8_t* payload, uint64_t len,
                       uint8_t** out, size_t* outLen) {
    if(!out || !outLen) return -1;
    *out = NULL; *outLen = 0;
    if(!payload && len > 0) return -1;
    if(opcode < 0 || opcode > 0xF) return -1;

    /* 头部长度：2 定长 + 扩展长度 + 4 掩码 */
    size_t headLen = 2 + (mask ? 4 : 0);
    if(len > 65535) headLen += 8;
    else if(len > 125) headLen += 2;

    size_t total = headLen + (size_t)len;
    uint8_t* buf = (uint8_t*)malloc(total ? total : 1);
    if(!buf) return -1;

    size_t p = 0;
    buf[p++] = (uint8_t)((fin ? 0x80 : 0x00) | (opcode & 0x0F));
    uint8_t maskBit = mask ? 0x80 : 0x00;
    if(len > 65535) {
        buf[p++] = maskBit | 127;
        for(int i = 7; i >= 0; i--)
            buf[p++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    } else if(len > 125) {
        buf[p++] = maskBit | 126;
        buf[p++] = (uint8_t)((len >> 8) & 0xFF);
        buf[p++] = (uint8_t)(len & 0xFF);
    } else {
        buf[p++] = maskBit | (uint8_t)len;
    }

    if(mask) {
        uint8_t key[4];
        wsMaskKey(key);
        memcpy(buf + p, key, 4);
        p += 4;
        for(uint64_t i = 0; i < len; i++)
            buf[p + i] = payload[i] ^ key[i % 4];
    } else if(len > 0) {
        memcpy(buf + p, payload, (size_t)len);
    }
    *out = buf;
    *outLen = total;
    return 0;
}

// ===== 解析器（句柄表 + 逐字节状态机） =====

#define LM_WS_MAX_PARSERS 1024

/* 状态：帧首字节 → 次字节 → 扩展长度 → 掩码 → 负载 */
enum {
    WS_ST_BYTE0 = 0,
    WS_ST_BYTE1,
    WS_ST_EXTLEN,
    WS_ST_MASK,
    WS_ST_PAYLOAD
};

typedef struct {
    int      inUse;
    uint32_t gen;            /* 代数校验（同 gzip 句柄表） */
    int      expectMasked;
    int      state;
    int      fin;
    int      opcode;
    int      masked;
    int      fragActive;     /* 分片消息进行中（已见 fin=0 数据帧） */
    uint64_t payloadLen;
    uint8_t  extBuf[8];      /* 扩展长度/掩码暂存 */
    int      extNeed;        /* 本阶段需要的字节数 */
    int      extGot;
    uint8_t  maskKey[4];
    uint8_t* payload;        /* malloc，容量 payloadLen */
    uint64_t payloadGot;
} LmWsParser;

static LmWsParser g_wsParsers[LM_WS_MAX_PARSERS];
static pthread_mutex_t g_wsLock = PTHREAD_MUTEX_INITIALIZER;

static int wsMakeHandle(int slot, uint32_t gen) { return slot * 4096 + (int)(gen % 4096); }

static LmWsParser* wsLookup(int h) {
    if(h < 0) return NULL;
    int slot = h / 4096;
    uint32_t gen = (uint32_t)(h % 4096);
    if(slot >= LM_WS_MAX_PARSERS) return NULL;
    LmWsParser* s = &g_wsParsers[slot];
    if(!s->inUse || (s->gen % 4096) != gen) return NULL;
    return s;
}

static void wsResetFrame(LmWsParser* s) {
    s->state = WS_ST_BYTE0;
    s->payload = NULL;
    s->payloadLen = 0;
    s->payloadGot = 0;
}

int lm_ws_parser_create(int expectMasked) {
    pthread_mutex_lock(&g_wsLock);
    int slot = -1;
    for(int i = 0; i < LM_WS_MAX_PARSERS; i++) {
        if(!g_wsParsers[i].inUse) { slot = i; break; }
    }
    if(slot < 0) {
        pthread_mutex_unlock(&g_wsLock);
        return -1;
    }
    LmWsParser* s = &g_wsParsers[slot];
    memset(s, 0, sizeof(*s));
    s->gen++;
    if(s->gen % 4096 == 0) s->gen++;
    s->inUse = 1;
    s->expectMasked = expectMasked ? 1 : 0;
    wsResetFrame(s);
    int h = wsMakeHandle(slot, s->gen);
    pthread_mutex_unlock(&g_wsLock);
    return h;
}

void lm_ws_parser_destroy(int h) {
    pthread_mutex_lock(&g_wsLock);
    LmWsParser* s = wsLookup(h);
    if(s) {
        free(s->payload);
        s->payload = NULL;
        s->inUse = 0;
    }
    pthread_mutex_unlock(&g_wsLock);
}

static int wsErr(char* errBuf, size_t errLen, const char* msg) {
    if(errBuf && errLen) snprintf(errBuf, errLen, "%s", msg);
    return -1;
}

/* 首字节校验：RSV 必为 0；opcode 合法；控制帧不可分片；分片秩序 */
static int wsCheckByte0(LmWsParser* s, uint8_t b, char* errBuf, size_t errLen) {
    if(b & 0x70)
        return wsErr(errBuf, errLen,
                     "ws 帧 RSV 位非 0（未协商扩展）/ ws frame RSV bits set without negotiated extension");
    int fin = (b & 0x80) ? 1 : 0;
    int op = b & 0x0F;
    switch(op) {
    case LM_WS_OP_CONT:
        if(!s->fragActive)
            return wsErr(errBuf, errLen,
                         "ws 续帧出现在分片消息之外 / ws continuation frame outside fragmented message");
        break;
    case LM_WS_OP_TEXT:
    case LM_WS_OP_BIN:
        if(s->fragActive)
            return wsErr(errBuf, errLen,
                         "ws 分片消息进行中又开新数据帧 / ws new data frame during fragmented message");
        break;
    case LM_WS_OP_CLOSE:
    case LM_WS_OP_PING:
    case LM_WS_OP_PONG:
        if(!fin)
            return wsErr(errBuf, errLen,
                         "ws 控制帧不可分片 / ws control frame must not be fragmented");
        break;
    default:
        return wsErr(errBuf, errLen,
                     "ws 帧 opcode 非法 / ws frame opcode invalid");
    }
    s->fin = fin;
    s->opcode = op;
    if(op == LM_WS_OP_TEXT || op == LM_WS_OP_BIN) {
        if(!fin) s->fragActive = 1;
    } else if(op == LM_WS_OP_CONT && fin) {
        s->fragActive = 0;
    }
    return 0;
}

/* 次字节与长度确定：掩码要求、控制帧长度、最小编码检查延后到扩展长度读齐 */
static int wsCheckByte1(LmWsParser* s, uint8_t b, char* errBuf, size_t errLen) {
    int masked = (b & 0x80) ? 1 : 0;
    if(s->expectMasked && !masked)
        return wsErr(errBuf, errLen,
                     "ws 客户端帧必须带掩码 / ws client frame must be masked");
    if(!s->expectMasked && masked)
        return wsErr(errBuf, errLen,
                     "ws 服务端帧不可带掩码 / ws server frame must not be masked");
    uint64_t len7 = b & 0x7F;
    if(s->opcode >= 0x8 && len7 > 125)
        return wsErr(errBuf, errLen,
                     "ws 控制帧负载超 125 字节 / ws control frame payload exceeds 125 bytes");
    s->masked = masked;
    s->payloadLen = len7;
    return 0;
}

int lm_ws_parser_feed(int h, const uint8_t* data, size_t len,
                      LmWsEvent** events, int* count,
                      char* errBuf, size_t errLen) {
    if(!events || !count) return -1;
    *events = NULL; *count = 0;
    LmWsParser* s = wsLookup(h);
    if(!s)
        return wsErr(errBuf, errLen,
                     "ws 解析器句柄无效 / invalid ws parser handle");

    int cap = 0;
    int n = 0;
    LmWsEvent* evs = NULL;
    size_t i = 0;
    while(i < len) {
        switch(s->state) {
        case WS_ST_BYTE0: {
            uint8_t b = data[i++];
            if(wsCheckByte0(s, b, errBuf, errLen) != 0) goto fail;
            s->state = WS_ST_BYTE1;
            break;
        }
        case WS_ST_BYTE1: {
            uint8_t b = data[i++];
            if(wsCheckByte1(s, b, errBuf, errLen) != 0) goto fail;
            if(s->payloadLen == 126) { s->extNeed = 2; s->extGot = 0; s->state = WS_ST_EXTLEN; }
            else if(s->payloadLen == 127) { s->extNeed = 8; s->extGot = 0; s->state = WS_ST_EXTLEN; }
            else if(s->masked) { s->extNeed = 4; s->extGot = 0; s->state = WS_ST_MASK; }
            else {
                s->state = WS_ST_PAYLOAD;
                if(s->payloadLen == 0) goto emit;   /* 空负载帧立即完成 */
                s->payload = (uint8_t*)malloc(s->payloadLen);
                if(!s->payload) { wsErr(errBuf, errLen, "ws 负载内存不足 / ws out of memory"); goto fail; }
            }
            break;
        }
        case WS_ST_EXTLEN: {
            while(i < len && s->extGot < s->extNeed)
                s->extBuf[s->extGot++] = data[i++];
            if(s->extGot < s->extNeed) break;   /* 等更多数据 */
            uint64_t v = 0;
            for(int k = 0; k < s->extNeed; k++)
                v = (v << 8) | s->extBuf[k];
            /* 最小编码与 2^63 上限（RFC 6455 §5.2） */
            if(s->payloadLen == 126 && v <= 125) {
                wsErr(errBuf, errLen,
                      "ws 长度未用最小编码（126 表示 <=125）/ ws non-minimal length encoding (126)");
                goto fail;
            }
            if(s->payloadLen == 127) {
                if(v <= 65535) {
                    wsErr(errBuf, errLen,
                          "ws 长度未用最小编码（127 表示 <=65535）/ ws non-minimal length encoding (127)");
                    goto fail;
                }
                if(v >> 63) {
                    wsErr(errBuf, errLen,
                          "ws 帧长最高位必须为 0 / ws frame length MSB must be 0");
                    goto fail;
                }
            }
            if(v > LM_WS_MAX_FRAME_PAYLOAD) {
                wsErr(errBuf, errLen,
                      "ws 帧负载超限 / ws frame payload too large");
                goto fail;
            }
            s->payloadLen = v;
            if(s->masked) { s->extNeed = 4; s->extGot = 0; s->state = WS_ST_MASK; }
            else {
                s->state = WS_ST_PAYLOAD;
                if(s->payloadLen == 0) goto emit;
                s->payload = (uint8_t*)malloc(s->payloadLen);
                if(!s->payload) { wsErr(errBuf, errLen, "ws 负载内存不足 / ws out of memory"); goto fail; }
            }
            break;
        }
        case WS_ST_MASK: {
            while(i < len && s->extGot < s->extNeed)
                s->maskKey[s->extGot++] = data[i++];
            if(s->extGot < s->extNeed) break;
            s->state = WS_ST_PAYLOAD;
            if(s->payloadLen == 0) goto emit;
            s->payload = (uint8_t*)malloc(s->payloadLen);
            if(!s->payload) { wsErr(errBuf, errLen, "ws 负载内存不足 / ws out of memory"); goto fail; }
            break;
        }
        case WS_ST_PAYLOAD: {
            uint64_t want = s->payloadLen - s->payloadGot;
            uint64_t avail = (uint64_t)(len - i);
            uint64_t take = want < avail ? want : avail;
            memcpy(s->payload + s->payloadGot, data + i, (size_t)take);
            i += (size_t)take;
            s->payloadGot += take;
            if(s->payloadGot < s->payloadLen) break;
            /* 掩码解除（RFC 6455 §5.3：payload[i] ^ maskKey[i % 4]） */
            if(s->masked) {
                for(uint64_t k = 0; k < s->payloadLen; k++)
                    s->payload[k] ^= s->maskKey[k % 4];
            }
            goto emit;
        }
        }
        continue;

    emit:;
        LmWsEvent ev;
        ev.fin = s->fin;
        ev.opcode = s->opcode;
        ev.payload = s->payload;
        ev.len = s->payloadLen;
        if(n == cap) {
            cap = cap ? cap * 2 : 8;
            LmWsEvent* grown = (LmWsEvent*)realloc(evs, sizeof(LmWsEvent) * (size_t)cap);
            if(!grown) {
                free(ev.payload);
                wsErr(errBuf, errLen, "ws 事件数组内存不足 / ws out of memory");
                goto fail;
            }
            evs = grown;
        }
        evs[n++] = ev;
        uint8_t* done = s->payload;
        (void)done;   /* 所有权移交事件 */
        wsResetFrame(s);
    }
    *events = evs;
    *count = n;
    return 0;

fail:
    for(int k = 0; k < n; k++) free(evs[k].payload);
    free(evs);
    free(s->payload);
    s->payload = NULL;
    return -1;
}
