// lm_wsq.h —— 工作窃取队列（Phase 8.2：Chase-Lev 无锁双端队列）
// 工作窃取队列的生产级语义：
//   - owner（本 scheduler 线程）从 bottom 端 push/pop（LIFO），热路径零 CAS；
//   - thief（其他 scheduler 线程）从 top 端 steal/steal_batch（FIFO），走 CAS；
//   - 容量为 2 的幂（默认 4096 槽，编译期 -DLM_WSQ_CAPACITY=n 可覆盖，
//     单测用小容量触发边界交错）。
//
// 并发契约（工作窃取队列）：
//   - push/pop/extract_tail_half 仅 owner 线程调用；
//   - steal_batch 可多 thief 并发，与 owner 的 push/pop 并发安全；
//   - 元素为 lm_co_t*（不透明指针），WSQ 不管理协程生命周期；
//   - 队列不丢不重由算法保证：steal 以 CAS 推 top 提交，CAS 失败者重读重试。
//
// 内存序（Chase-Lev 标准）：
//   push：写槽 → release fence → bottom+1（relaxed）；
//   pop ：bottom-1（relaxed）→ seq_cst fence → 读 top → 末元素与 thief CAS 竞争；
//   steal：acquire 读 top → acquire 读 bottom → 读槽 → CAS(top)（acq_rel）提交。
#ifndef LM_WSQ_H
#define LM_WSQ_H

#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef LM_WSQ_CAPACITY
#define LM_WSQ_CAPACITY 4096   /* 必须 2 的幂 */
#endif

/* 单批窃取上限默认值（参数化，环境变量/调优入口在 lm_scheduler.c） */
#ifndef LM_WSQ_STEAL_MAX_BATCH
#define LM_WSQ_STEAL_MAX_BATCH 32
#endif

/* 窃取重试上限（4 次重试） */
#ifndef LM_WSQ_STEAL_TRIES
#define LM_WSQ_STEAL_TRIES 4
#endif

/* bottom/top 用有符号 int64_t：
 * Chase-Lev pop 的 bottom-1 在空队列初始态（bottom==0）会产生 -1，
 * 有符号下 t > b 判定成立归位 NULL；无符号回绕 SIZE_MAX 会误判非空。
 * 单调递增不回绕需 2^63 次操作，工程上不可达。 */
typedef struct lm_wsq_s {
    _Atomic int64_t bottom;  /* owner push/pop 端（LIFO，仅 owner 写） */
    _Atomic int64_t top;     /* thief steal 端（FIFO，CAS 推进） */
    size_t capacity;         /* 槽位数（2 的幂） */
    size_t mask;             /* capacity - 1 */
    void** buffer;           /* 堆分配槽位数组（lm_co_t* 元素） */
} lm_wsq_t;

/* 初始化：分配 capacity 槽缓冲。cap=0 用 LM_WSQ_CAPACITY。
 * cap 非 2 的幂时向上取整到 2 的幂。返回 0 成功 / -1 失败。 */
int  lm_wsq_init(lm_wsq_t* q, size_t cap);

/* 销毁：释放槽缓冲（不处理残留元素，owner 负责排空）。 */
void lm_wsq_destroy(lm_wsq_t* q);

/* owner push 到 bottom 端（LIFO）。队列满返回 -1（调用方走溢出路径），成功 0。
 * 仅 owner 线程调用。 */
int  lm_wsq_push(lm_wsq_t* q, void* co);

/* owner 从 bottom 端 pop（LIFO）。空返回 NULL。仅 owner 线程调用。 */
void* lm_wsq_pop(lm_wsq_t* q);

/* owner 抽取 bottom 侧至多一半元素到 out[]（溢出灌全局用，溢出协议
 * 选后半段：全局取回的任务放本地前半段，溢出走后半段，防本地↔全局反复弹跳）。
 * 返回抽取个数（0 = 队列太空不抽）。仅 owner 线程调用。 */
size_t lm_wsq_extract_tail_half(lm_wsq_t* q, void** out, size_t max_out);

/* thief 批量窃取：快照 top/bottom，取 n=(len+1)/2（上限 max_batch），
 * 拷贝到 out[] 后 CAS 推 top 提交；快照不一致/CAS 失败重试（上限 LM_WSQ_STEAL_TRIES）。
 * 返回窃取个数（0 = 空或重试耗尽）。max_batch=0 用 LM_WSQ_STEAL_MAX_BATCH。
 * 可多 thief 并发调用。 */
size_t lm_wsq_steal_batch(lm_wsq_t* q, void** out, size_t max_batch);

/* 快照长度（top..bottom 差值，瞬时近似值，仅供监控/判空快查）。 */
static inline long lm_wsq_size_approx(const lm_wsq_t* q) {
    int64_t t = atomic_load_explicit((_Atomic int64_t*)&q->top, memory_order_acquire);
    int64_t b = atomic_load_explicit((_Atomic int64_t*)&q->bottom, memory_order_acquire);
    return (b > t) ? (long)(b - t) : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* LM_WSQ_H */
