// lm_sync.h —— 同步原语家族（Phase 8.9）
// 全部建在 lm_butex 之上：原语层只维护各自状态机，睡眠/唤醒一律 butex——
// 一个原语优化（无 waiter 不 syscall）全家受益。
// 对标 bthread butex 全家（mutex/semaphore/rwlock/countdown/once）+
// Go sudog/semaRoot。
//
// 硬性要求（全族遵守）：
//   - 无 waiter 不 syscall（butex_wake 无 waiter 时 0 syscall，原语层继承）
//   - 协程/线程上下文混用安全（butex 内部按 lm_co_current 分流）
//   - TSAN 干净（全原子操作，内存序显式标注）
//   - waiter 队列 O(1)（butex 哈希分桶）
//   - 自旋默认关闭、参数化保留（lumyr 场景竞争预期低，对齐 Go sync.Mutex
//     有限自旋思路但默认不走——见 lm_sync_set_spin）
#ifndef LM_SYNC_H
#define LM_SYNC_H

#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * 协程 mutex：32 位三态（0=未锁 / 1=已锁无等待 / 2=已锁有等待）
 * 无竞争一次原子交换得锁；竞争进 butex 等待队列。
 * 对齐 bthread mutex.cpp 与 glibc/musl futex mutex 的 0/1/2 模式。
 * ============================================================ */
typedef struct {
    _Atomic uint32_t word;
} lm_mutex_t;

void lm_mutex_init(lm_mutex_t* m);
void lm_mutex_lock(lm_mutex_t* m);
/* 非阻塞尝试：得锁返回 1，否则 0。 */
int  lm_mutex_trylock(lm_mutex_t* m);
void lm_mutex_unlock(lm_mutex_t* m);

/* ============================================================
 * semaphore：计数原子 + butex 等待（对齐 bthread semaphore.cpp）
 * 用途：连接数上限、资源池限流。
 * ============================================================ */
typedef struct {
    _Atomic uint32_t count;   /* 可用资源数 */
} lm_sem_t;

void lm_sem_init(lm_sem_t* s, uint32_t initial);
/* P 操作：取一个资源，无则 butex 等待。 */
void lm_sem_wait(lm_sem_t* s);
/* 非阻塞尝试：取到返回 1，否则 0。 */
int  lm_sem_trywait(lm_sem_t* s);
/* V 操作：还一个资源，有等待者则 wake 一个。 */
void lm_sem_post(lm_sem_t* s);

/* ============================================================
 * rwlock：读者计数 + 写者 butex 排队（对齐 bthread rwlock.cpp）
 * 读多写少共享配置/缓存场景。
 * word 编码：bit31 = 写者持有标记，低 31 位 = 活跃读者数。
 * ============================================================ */
typedef struct {
    _Atomic uint32_t word;
} lm_rwlock_t;

void lm_rwlock_init(lm_rwlock_t* l);
void lm_rwlock_rdlock(lm_rwlock_t* l);
void lm_rwlock_rdunlock(lm_rwlock_t* l);
void lm_rwlock_wrlock(lm_rwlock_t* l);
void lm_rwlock_wrunlock(lm_rwlock_t* l);

/* ============================================================
 * countdown / WaitGroup：计数器 + butex，归零 wake_all
 * （对齐 bthread countdown_event.cpp）。用途：fork-join 汇聚。
 * ============================================================ */
typedef struct {
    _Atomic uint32_t count;   /* 剩余未完成数 */
} lm_countdown_t;

void lm_countdown_init(lm_countdown_t* c, uint32_t n);
/* 完成一个：计数 -1，归零时 wake_all 唤醒全部等待者。 */
void lm_countdown_done(lm_countdown_t* c);
/* 等待计数归零。 */
void lm_countdown_wait(lm_countdown_t* c);

/* ============================================================
 * once：butex 一次性等待（惰性初始化）
 * word 三态：0=未执行 / 1=执行中 / 2=已完成。
 * ============================================================ */
typedef struct {
    _Atomic uint32_t word;
} lm_once_t;

typedef void (*lm_once_fn_t)(void* arg);

void lm_once_init(lm_once_t* o);
/* 保证 fn 全进程只被执行一次：首个到达者执行，其余 butex 等待其完成。 */
void lm_once_call(lm_once_t* o, lm_once_fn_t fn, void* arg);

/* ============================================================
 * 自旋策略（默认关闭，参数化保留）：设置竞争时进入 butex 前的有限自旋
 * 次数。对齐 Go sync.Mutex 思路（有限自旋仅多核有意义），lumyr 竞争
 * 预期低故默认 0。传 n>0 开启（建议 <= 4）。
 * ============================================================ */
void lm_sync_set_spin(int n);

#ifdef __cplusplus
}
#endif

#endif // LM_SYNC_H
