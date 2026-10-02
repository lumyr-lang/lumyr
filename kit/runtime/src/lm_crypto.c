// lm_crypto.c —— URL 编码/解码、Base64、MD5
#include "lm_crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

// ===== URL 编码/解码 =====
char* lumyr_url_encode(const char* s) {
    if(!s) return strdup("");
    size_t cap = strlen(s) * 3 + 1;
    char* out = malloc(cap);
    if(!out) return NULL;
    char* p = out;
    for(const unsigned char* q = (const unsigned char*)s; *q; q++) {
        if(isalnum(*q) || *q=='-' || *q=='_' || *q=='.' || *q=='~' || *q >= 0x80) {
            *p++ = (char)*q;
        } else {
            p += snprintf(p, 4, "%%%02X", *q);
        }
    }
    *p = 0;
    return out;
}

char* lumyr_url_decode(const char* s) {
    if(!s) return strdup("");
    char* out = malloc(strlen(s) + 1);
    if(!out) return NULL;
    char* p = out;
    for(const char* q = s; *q; q++) {
        if(*q == '%' && q[1] && q[2]) {
            int v = 0;
            if(sscanf(q + 1, "%2x", &v) == 1) { *p++ = (char)v; q += 2; }
            else *p++ = *q;
        } else if(*q == '+') *p++ = ' ';
        else *p++ = *q;
    }
    *p = 0;
    return out;
}

// ===== Base64 =====
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char* lumyr_base64_encode(const char* s, int len) {
    if(len < 0) len = (int)strlen(s ? s : "");
    int outlen = ((len + 2) / 3) * 4;
    char* out = malloc(outlen + 1);
    if(!out) return NULL;
    int i, j = 0;
    for(i = 0; i + 2 < len; i += 3) {
        unsigned int v = ((unsigned char)s[i] << 16) | ((unsigned char)s[i+1] << 8) | (unsigned char)s[i+2];
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = b64_table[(v >> 6) & 0x3F];
        out[j++] = b64_table[v & 0x3F];
    }
    if(i < len) {
        unsigned int v = (unsigned char)s[i] << 16;
        if(i + 1 < len) v |= (unsigned char)s[i+1] << 8;
        out[j++] = b64_table[(v >> 18) & 0x3F];
        out[j++] = b64_table[(v >> 12) & 0x3F];
        out[j++] = (i + 1 < len) ? b64_table[(v >> 6) & 0x3F] : '=';
        out[j++] = '=';
    }
    out[j] = 0;
    return out;
}

static int b64_val(char c) {
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

char* lumyr_base64_decode(const char* s, int* outlen) {
    if(!s) { if(outlen) *outlen = 0; return strdup(""); }
    int slen = (int)strlen(s);
    char* out = malloc(slen + 1);
    if(!out) { if(outlen) *outlen = 0; return NULL; }
    int j = 0;
    unsigned int buf = 0;
    int bits = 0;
    for(int i = 0; i < slen; i++) {
        if(s[i] == '=') break;
        int v = b64_val(s[i]);
        if(v < 0) continue;
        buf = (buf << 6) | v;
        bits += 6;
        if(bits >= 8) {
            bits -= 8;
            out[j++] = (char)((buf >> bits) & 0xFF);
        }
    }
    out[j] = 0;
    if(outlen) *outlen = j;
    return out;
}

// ===== MD5 (RFC 1321) =====
typedef struct {
    unsigned int state[4];
    unsigned long long count;
    unsigned char buffer[64];
} MD5_CTX;

#define F(x,y,z) ((x & y) | (~x & z))
#define G(x,y,z) ((x & z) | (y & ~z))
#define H(x,y,z) (x ^ y ^ z)
#define I(x,y,z) (y ^ (x | ~z))
#define ROTL(x,n) ((x << n) | (x >> (32 - n)))
#define FF(a,b,c,d,x,s,ac) { a += F(b,c,d) + x + ac; a = ROTL(a,s); a += b; }
#define GG(a,b,c,d,x,s,ac) { a += G(b,c,d) + x + ac; a = ROTL(a,s); a += b; }
#define HH(a,b,c,d,x,s,ac) { a += H(b,c,d) + x + ac; a = ROTL(a,s); a += b; }
#define II(a,b,c,d,x,s,ac) { a += I(b,c,d) + x + ac; a = ROTL(a,s); a += b; }

static void md5_transform(unsigned int state[4], const unsigned char block[64]) {
    unsigned int a = state[0], b = state[1], c = state[2], d = state[3], x[16];
    for(int i = 0; i < 16; i++)
        x[i] = ((unsigned int)block[i*4]) | ((unsigned int)block[i*4+1] << 8) |
               ((unsigned int)block[i*4+2] << 16) | ((unsigned int)block[i*4+3] << 24);
    FF(a,b,c,d,x[0], 7,0xd76aa478); FF(d,a,b,c,x[1],12,0xe8c7b756); FF(c,d,a,b,x[2],17,0x242070db); FF(b,c,d,a,x[3],22,0xc1bdceee);
    FF(a,b,c,d,x[4], 7,0xf57c0faf); FF(d,a,b,c,x[5],12,0x4787c62a); FF(c,d,a,b,x[6],17,0xa8304613); FF(b,c,d,a,x[7],22,0xfd469501);
    FF(a,b,c,d,x[8], 7,0x698098d8); FF(d,a,b,c,x[9],12,0x8b44f7af); FF(c,d,a,b,x[10],17,0xffff5bb1); FF(b,c,d,a,x[11],22,0x895cd7be);
    FF(a,b,c,d,x[12],7,0x6b901122); FF(d,a,b,c,x[13],12,0xfd987193); FF(c,d,a,b,x[14],17,0xa679438e); FF(b,c,d,a,x[15],22,0x49b40821);
    GG(a,b,c,d,x[1], 5,0xf61e2562); GG(d,a,b,c,x[6], 9,0xc040b340); GG(c,d,a,b,x[11],14,0x265e5a51); GG(b,c,d,a,x[0],20,0xe9b6c7aa);
    GG(a,b,c,d,x[5], 5,0xd62f105d); GG(d,a,b,c,x[10],9,0x02441453); GG(c,d,a,b,x[15],14,0xd8a1e681); GG(b,c,d,a,x[4],20,0xe7d3fbc8);
    GG(a,b,c,d,x[9], 5,0x21e1cde6); GG(d,a,b,c,x[14],9,0xc33707d6); GG(c,d,a,b,x[3],14,0xf4d50d87); GG(b,c,d,a,x[8],20,0x455a14ed);
    GG(a,b,c,d,x[13],5,0xa9e3e905); GG(d,a,b,c,x[2], 9,0xfcefa3f8); GG(c,d,a,b,x[7],14,0x676f02d9); GG(b,c,d,a,x[12],20,0x8d2a4c8a);
    HH(a,b,c,d,x[5], 4,0xfffa3942); HH(d,a,b,c,x[8],11,0x8771f681); HH(c,d,a,b,x[11],16,0x6d9d6122); HH(b,c,d,a,x[14],23,0xfde5380c);
    HH(a,b,c,d,x[1], 4,0xa4beea44); HH(d,a,b,c,x[4],11,0x4bdecfa9); HH(c,d,a,b,x[7],16,0xf6bb4b60); HH(b,c,d,a,x[10],23,0xbebfbc70);
    HH(a,b,c,d,x[13],4,0x289b7ec6); HH(d,a,b,c,x[0],11,0xeaa127fa); HH(c,d,a,b,x[3],16,0xd4ef3085); HH(b,c,d,a,x[6],23,0x04881d05);
    HH(a,b,c,d,x[9], 4,0xd9d4d039); HH(d,a,b,c,x[12],11,0xe6db99e5); HH(c,d,a,b,x[15],16,0x1fa27cf8); HH(b,c,d,a,x[2],23,0xc4ac5665);
    II(a,b,c,d,x[0], 6,0xf4292244); II(d,a,b,c,x[7],10,0x432aff97); II(c,d,a,b,x[14],15,0xab9423a7); II(b,c,d,a,x[5],21,0xfc93a039);
    II(a,b,c,d,x[12],6,0x655b59c3); II(d,a,b,c,x[3],10,0x8f0ccc92); II(c,d,a,b,x[10],15,0xffeff47d); II(b,c,d,a,x[1],21,0x85845dd1);
    II(a,b,c,d,x[8], 6,0x6fa87e4f); II(d,a,b,c,x[15],10,0xfe2ce6e0); II(c,d,a,b,x[6],15,0xa3014314); II(b,c,d,a,x[13],21,0x4e0811a1);
    II(a,b,c,d,x[4], 6,0xf7537e82); II(d,a,b,c,x[11],10,0xbd3af235); II(c,d,a,b,x[2],15,0x2ad7d2bb); II(b,c,d,a,x[9],21,0xeb86d391);
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
}

static void md5_init(MD5_CTX* c) {
    c->state[0]=0x67452301; c->state[1]=0xefcdab89; c->state[2]=0x98badcfe; c->state[3]=0x10325476;
    c->count=0;
}

static void md5_update(MD5_CTX* c, const unsigned char* data, int len) {
    int idx = (int)((c->count >> 3) & 0x3F);
    c->count += (unsigned long long)len << 3;
    int fill = 64 - idx;
    int i = 0;
    if(len >= fill) {
        memcpy(&c->buffer[idx], data, fill);
        md5_transform(c->state, c->buffer);
        for(i = fill; i + 63 < len; i += 64) md5_transform(c->state, &data[i]);
        idx = 0;
    }
    memcpy(&c->buffer[idx], &data[i], len - i);
}

static void md5_final(MD5_CTX* c, unsigned char digest[16]) {
    unsigned char bits[8];
    for(int i = 0; i < 8; i++) bits[i] = (unsigned char)((c->count >> (i*8)) & 0xFF);
    int idx = (int)((c->count >> 3) & 0x3F);
    int padlen = (idx < 56) ? (56 - idx) : (120 - idx);
    static unsigned char PADDING[64] = {0x80};
    md5_update(c, PADDING, padlen);
    md5_update(c, bits, 8);
    for(int i = 0; i < 4; i++) {
        digest[i*4]   = (unsigned char)(c->state[i] & 0xFF);
        digest[i*4+1] = (unsigned char)((c->state[i] >> 8) & 0xFF);
        digest[i*4+2] = (unsigned char)((c->state[i] >> 16) & 0xFF);
        digest[i*4+3] = (unsigned char)((c->state[i] >> 24) & 0xFF);
    }
}

char* lumyr_md5_hex(const char* s, int len) {
    if(len < 0) len = (int)strlen(s ? s : "");
    MD5_CTX ctx;
    unsigned char digest[16];
    md5_init(&ctx);
    md5_update(&ctx, (const unsigned char*)(s ? s : ""), len);
    md5_final(&ctx, digest);
    char* out = malloc(33);
    if(!out) return NULL;
    for(int i = 0; i < 16; i++) snprintf(out + i*2, 3, "%02x", digest[i]);
    out[32] = 0;
    return out;
}

// ===== SHA-1 (RFC 3174，一次性摘要) =====
// 按 RFC 3174 第 7 节算法自写实现：消息调度 w[t] 与四轮 f/K 常量均为
// 标准公开规格。供 WebSocket 握手（Sec-WebSocket-Accept）等场景使用。

/* 单个 64 字节分块压缩：w 调度 80 步，四轮各自的 f 函数与 K 常量 */
static void sha1_compress(uint32_t st[5], const uint8_t block[64]) {
    uint32_t w[80];
    for(int t = 0; t < 16; t++)
        w[t] = ((uint32_t)block[t*4] << 24) | ((uint32_t)block[t*4+1] << 16) |
               ((uint32_t)block[t*4+2] << 8) | (uint32_t)block[t*4+3];
    for(int t = 16; t < 80; t++) {
        uint32_t v = w[t-3] ^ w[t-8] ^ w[t-14] ^ w[t-16];
        w[t] = (v << 1) | (v >> 31);
    }
    uint32_t a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for(int t = 0; t < 80; t++) {
        uint32_t f, k;
        if(t < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if(t < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if(t < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else            { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t tmp = ((a << 5) | (a >> 27)) + f + e + k + w[t];
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = tmp;
    }
    st[0]+=a; st[1]+=b; st[2]+=c; st[3]+=d; st[4]+=e;
}

void lumyr_sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
    uint32_t st[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
    const uint8_t* p = data;
    size_t left = len;
    while(left >= 64) { sha1_compress(st, p); p += 64; left -= 64; }

    /* 收尾：补 0x80、零填充至距块尾 8 字节，再附 64 位大端位长 */
    uint8_t tail[128];
    memset(tail, 0, sizeof(tail));
    if(left) memcpy(tail, p, left);
    tail[left] = 0x80;
    size_t tailLen = (left < 56) ? 64 : 128;
    uint64_t bitLen = (uint64_t)len * 8;
    for(int i = 0; i < 8; i++)
        tail[tailLen - 1 - i] = (uint8_t)((bitLen >> (i * 8)) & 0xFF);
    sha1_compress(st, tail);
    if(tailLen == 128) sha1_compress(st, tail + 64);

    for(int i = 0; i < 5; i++) {
        out[i*4]   = (uint8_t)((st[i] >> 24) & 0xFF);
        out[i*4+1] = (uint8_t)((st[i] >> 16) & 0xFF);
        out[i*4+2] = (uint8_t)((st[i] >> 8) & 0xFF);
        out[i*4+3] = (uint8_t)(st[i] & 0xFF);
    }
}
