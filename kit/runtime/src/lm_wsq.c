// lm_wsq.c —— Chase-Lev 无锁工作窃取队列实现（Phase 8.2）
// 算法参照 "Dynamic Circular Work-Stealing Deque"（Chase & Lev 2005）定长版：
// 定长环形缓冲 + bottom/top 单调递增索引（mask 取槽位），
// owner 热路径无 CAS，thief 以 CAS(top) 提交窃取。
//
// 索引用有符号 int64_t（对齐 bthread 的 int）：pop 的 bottom-1 在空队列
// 初始态（bottom==0）产生 -1，有符号下 t > b 判定成立正确归位；
// 无符号回绕会误判非空。2^63 次操作才回绕，工程上不可达。
#include "lm_wsq.h"
#include <stdlib.h>

int lm_wsq_init(lm_wsq_t* q, size_t cap) {
    if (!q) return -1;
    if (cap == 0) cap = LM_WSQ_CAPACITY;
    /* 向上取整到 2 的幂 */
    size_t c = 1;
    while (c < cap) c <<= 1;
    q->buffer = (void**)calloc(c, sizeof(void*));
    if (!q->buffer) return -1;
    atomic_store_explicit(&q->bottom, 0, memory_order_relaxed);
    atomic_store_explicit(&q->top, 0, memory_order_relaxed);
    q->capacity = c;
    q->mask = c - 1;
    return 0;
}

void lm_wsq_destroy(lm_wsq_t* q) {
    if (!q) return;
    free(q->buffer);
    q->buffer = NULL;
    q->capacity = 0;
    q->mask = 0;
}

int lm_wsq_push(lm_wsq_t* q, void* co) {
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    if ((size_t)(b - t) >= q->capacity) return -1;   /* 满：调用方走溢出路径 */
    q->buffer[(size_t)b & q->mask] = co;
    /* 槽位写先于 bottom 推进对 thief 可见（steal 读 bottom 后读槽位） */
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bottom, b + 1, memory_order_relaxed);
    return 0;
}

void* lm_wsq_pop(lm_wsq_t* q) {
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    b = b - 1;
    atomic_store_explicit(&q->bottom, b, memory_order_relaxed);
    /* seq_cst fence：保证 bottom 回退先于 top 读取全局可见，
     * 与 steal 的 CAS(top) 形成全序，防 owner/thief 同时取走末元素 */
    atomic_thread_fence(memory_order_seq_cst);
    int64_t t = atomic_load_explicit(&q->top, memory_order_relaxed);
    if (t > b) {
        /* 队列空（含初始态 b 回退到 -1）：bottom 归位到 top（标准 Chase-Lev） */
        atomic_store_explicit(&q->bottom, t, memory_order_relaxed);
        return NULL;
    }
    void* co = q->buffer[(size_t)b & q->mask];
    if (t == b) {
        /* 末元素：与并发 steal 竞争，CAS 抢 top 推进权 */
        int64_t expected = t;
        if (!atomic_compare_exchange_strong_explicit(&q->top, &expected, t + 1,
                                                     memory_order_acq_rel,
                                                     memory_order_relaxed)) {
            co = NULL;   /* 被 thief 抢走 */
        }
        atomic_store_explicit(&q->bottom, t + 1, memory_order_relaxed);
    }
    return co;
}

size_t lm_wsq_extract_tail_half(lm_wsq_t* q, void** out, size_t max_out) {
    int64_t b = atomic_load_explicit(&q->bottom, memory_order_relaxed);
    int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
    int64_t n = (b > t) ? (b - t) : 0;
    if (n < 2) return 0;   /* 少于 2 个不值得抽（留 1 个本地跑） */
    size_t half = (size_t)n / 2;
    if (half > max_out) half = max_out;
    /* 抽 bottom 侧（最新 push 的一半），槽位拷贝后 bottom 回退。
     * owner 独占 bottom 写，无 CAS；thief 只看到 top 侧（不受影响）。 */
    for (size_t i = 0; i < half; i++) {
        out[i] = q->buffer[(size_t)(b - (int64_t)half + (int64_t)i) & q->mask];
    }
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&q->bottom, b - (int64_t)half, memory_order_relaxed);
    return half;
}

size_t lm_wsq_steal_batch(lm_wsq_t* q, void** out, size_t max_batch) {
    if (!q || !out) return 0;
    if (max_batch == 0) max_batch = LM_WSQ_STEAL_MAX_BATCH;
    for (int tries = 0; tries < LM_WSQ_STEAL_TRIES; tries++) {
        int64_t t = atomic_load_explicit(&q->top, memory_order_acquire);
        int64_t b = atomic_load_explicit(&q->bottom, memory_order_acquire);
        if (t >= b) return 0;   /* 空 */
        size_t n = (size_t)(b - t);
        n = (n + 1) / 2;        /* n - n/2 向上取整一半（对齐 Go runqgrab） */
        if (n > max_batch) n = max_batch;
        /* CAS 提交前读槽位：槽位内容先于 bottom 推进写入（push 的 release fence），
         * acquire 读 bottom 保证看到槽位已写。读到的槽位若被并发 pop 取走
         * （仅末元素竞争场景），CAS 会因 top 被 pop 推进而失败 → 重试。 */
        for (size_t i = 0; i < n; i++) {
            out[i] = q->buffer[(size_t)(t + (int64_t)i) & q->mask];
        }
        int64_t expected = t;
        if (atomic_compare_exchange_strong_explicit(&q->top, &expected,
                                                    t + (int64_t)n,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            return n;
        }
        /* CAS 失败：快照已被并发操作改变，重读重试 */
    }
    return 0;   /* 重试耗尽（高竞争），调用方换 victim 或回落 */
}
