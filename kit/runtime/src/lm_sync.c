// lm_sync.c —— 同步原语家族实现（Phase 8.9）
// 全部建在 lm_butex 之上：原语层只维护各自状态机，睡眠/唤醒一律 butex。
// 丢唤醒防护依赖 butex 契约：等待方入队时在桶锁内 recheck 原子字，
// 唤醒方「先改字、后 wake」，二者配对关闭窗口（见 lm_butex.h 头注）。
#include "lm_sync.h"
#include "lm_butex.h"

#include <sched.h>   /* sched_yield（自旋让出） */

/* 竞争时进入 butex 前的有限自旋次数（默认 0 = 不自旋，直接排队）。
 * lumyr 场景竞争预期低，有限自旋思路但默认关闭、参数化保留。 */
static int g_spin = 0;

void lm_sync_set_spin(int n) { g_spin = (n > 0) ? n : 0; }

/* 有限自旋辅助：返回非 0 表示自旋期间条件可能已满足，调用方应重试快路径。 */
static int spin_wait(int* spins) {
    if (g_spin <= 0) return 0;
    if (++(*spins) >= g_spin) { *spins = 0; return 0; }
    sched_yield();
    return 1;
}

/* ============================================================
 * mutex：32 位三态（0=未锁 / 1=已锁无等待 / 2=已锁有等待）
 * 对齐 musl/glibc futex mutex：
 *   lock 快路径  CAS 0→1；
 *   慢路径抢到后  CAS 0→2（保持「有等待」标记，unlock 据此 wake 传递链）；
 *   unlock       exchange 0，旧值==2（有等待者）才 butex_wake（无 waiter 不 syscall）。
 * ============================================================ */
void lm_mutex_init(lm_mutex_t* m) {
    atomic_store_explicit(&m->word, 0, memory_order_relaxed);
}

void lm_mutex_lock(lm_mutex_t* m) {
    /* 快路径：无竞争一次原子交换 */
    uint32_t exp = 0;
    if (atomic_compare_exchange_strong_explicit(&m->word, &exp, 1,
            memory_order_acquire, memory_order_relaxed)) {
        return;
    }
    /* 慢路径：竞争，进 butex */
    int spins = 0;
    for (;;) {
        uint32_t v = atomic_load_explicit(&m->word, memory_order_relaxed);
        if (v == 0) {
            /* 锁被释放：抢到，置 2 保持「有等待」标记（可能仍有他者在睡） */
            exp = 0;
            if (atomic_compare_exchange_strong_explicit(&m->word, &exp, 2,
                    memory_order_acquire, memory_order_relaxed)) {
                return;
            }
            continue;
        }
        /* 锁被占：有限自旋（默认关）后标记有等待并入睡 */
        if (spin_wait(&spins)) continue;
        if (v != 2) {
            exp = v;
            if (!atomic_compare_exchange_strong_explicit(&m->word, &exp, 2,
                    memory_order_relaxed, memory_order_relaxed)) {
                continue;   /* 值变了重试 */
            }
        }
        /* 睡到被 wake；醒后（含伪唤醒）回到循环重抢 */
        lm_butex_wait(&m->word, 2);
    }
}

int lm_mutex_trylock(lm_mutex_t* m) {
    uint32_t exp = 0;
    return atomic_compare_exchange_strong_explicit(&m->word, &exp, 1,
            memory_order_acquire, memory_order_relaxed);
}

void lm_mutex_unlock(lm_mutex_t* m) {
    /* exchange 0：旧值==2 说明有等待者在睡，wake 一个传递锁；
     * 旧值==1 无等待者，不发 syscall（S4：无 waiter 不唤醒）。 */
    if (atomic_exchange_explicit(&m->word, 0, memory_order_release) == 2) {
        lm_butex_wake(&m->word, 1);
    }
}

/* ============================================================
 * semaphore：count>0 可取；取到 0 时等待者 butex 睡在 count==0 上。
 * post 先 fetch_add 改字、后 wake——与 wait 入队 recheck 配对防丢唤醒。
 * ============================================================ */
void lm_sem_init(lm_sem_t* s, uint32_t initial) {
    atomic_store_explicit(&s->count, initial, memory_order_relaxed);
}

void lm_sem_wait(lm_sem_t* s) {
    int spins = 0;
    for (;;) {
        uint32_t v = atomic_load_explicit(&s->count, memory_order_acquire);
        if (v > 0) {
            uint32_t exp = v;
            if (atomic_compare_exchange_strong_explicit(&s->count, &exp, v - 1,
                    memory_order_acquire, memory_order_relaxed)) {
                return;
            }
            continue;
        }
        /* v == 0：无资源，睡到 post；recheck 在 butex 桶锁内，丢唤醒已关闭 */
        if (spin_wait(&spins)) continue;
        lm_butex_wait(&s->count, 0);
    }
}

int lm_sem_trywait(lm_sem_t* s) {
    for (;;) {
        uint32_t v = atomic_load_explicit(&s->count, memory_order_acquire);
        if (v == 0) return 0;
        uint32_t exp = v;
        if (atomic_compare_exchange_strong_explicit(&s->count, &exp, v - 1,
                memory_order_acquire, memory_order_relaxed)) {
            return 1;
        }
    }
}

void lm_sem_post(lm_sem_t* s) {
    /* 先改字（+1）再 wake：等待方 recheck 见 count>0 不睡，与入队配对 */
    atomic_fetch_add_explicit(&s->count, 1, memory_order_release);
    lm_butex_wake(&s->count, 1);
}

/* ============================================================
 * rwlock：bit31 = 写者持有标记，低 31 位 = 活跃读者数。
 *   读者：见写者标记则 butex 等；否则 CAS +1。
 *   写者：见非 0 则 butex 等；否则 CAS 0→WRITER 独占。
 *   解锁：读者 -1 归零 wake_all（唤醒等待写者/读者）；
 *         写者清标记归零 wake_all。
 * 注：基础公平版（无写优先防饥饿），读多写少场景适用；写饥饿防护后续增强。
 * ============================================================ */
#define LM_RW_WRITER 0x80000000u
#define LM_RW_RMASK  0x7fffffffu

void lm_rwlock_init(lm_rwlock_t* l) {
    atomic_store_explicit(&l->word, 0, memory_order_relaxed);
}

void lm_rwlock_rdlock(lm_rwlock_t* l) {
    for (;;) {
        uint32_t v = atomic_load_explicit(&l->word, memory_order_acquire);
        if (v & LM_RW_WRITER) {
            lm_butex_wait(&l->word, v);   /* 有写者：等 */
            continue;
        }
        uint32_t exp = v;
        if (atomic_compare_exchange_strong_explicit(&l->word, &exp, v + 1,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
    }
}

void lm_rwlock_rdunlock(lm_rwlock_t* l) {
    /* -1 后归零：可能有写者/读者在等，wake_all */
    uint32_t old = atomic_fetch_sub_explicit(&l->word, 1, memory_order_release);
    if ((old & LM_RW_RMASK) == 1) {
        lm_butex_wake_all(&l->word);
    }
}

void lm_rwlock_wrlock(lm_rwlock_t* l) {
    for (;;) {
        uint32_t v = atomic_load_explicit(&l->word, memory_order_acquire);
        if (v != 0) {
            lm_butex_wait(&l->word, v);   /* 有读者/写者：等 */
            continue;
        }
        uint32_t exp = 0;
        if (atomic_compare_exchange_strong_explicit(&l->word, &exp, LM_RW_WRITER,
                memory_order_acquire, memory_order_relaxed)) {
            return;
        }
    }
}

void lm_rwlock_wrunlock(lm_rwlock_t* l) {
    atomic_store_explicit(&l->word, 0, memory_order_release);
    lm_butex_wake_all(&l->word);
}

/* ============================================================
 * countdown / WaitGroup：count 归零时 wake_all。
 * wait 循环读当前值作 butex expected——值变（done）立即返回重读，
 * 归零则退出。防丢唤醒：done 的 fetch_sub 先于 wake_all。
 * ============================================================ */
void lm_countdown_init(lm_countdown_t* c, uint32_t n) {
    atomic_store_explicit(&c->count, n, memory_order_relaxed);
}

void lm_countdown_done(lm_countdown_t* c) {
    uint32_t old = atomic_fetch_sub_explicit(&c->count, 1, memory_order_acq_rel);
    if (old == 1) {
        lm_butex_wake_all(&c->count);   /* 归零：唤醒全部等待者 */
    }
}

void lm_countdown_wait(lm_countdown_t* c) {
    for (;;) {
        uint32_t v = atomic_load_explicit(&c->count, memory_order_acquire);
        if (v == 0) return;
        lm_butex_wait(&c->count, v);   /* v 变（done）立即返回重读 */
    }
}

/* ============================================================
 * once：0=未执行 / 1=执行中 / 2=已完成。
 * 首个 CAS 0→1 者执行 fn 后 store 2 + wake_all；其余 butex 等待至 2。
 * ============================================================ */
void lm_once_init(lm_once_t* o) {
    atomic_store_explicit(&o->word, 0, memory_order_relaxed);
}

void lm_once_call(lm_once_t* o, lm_once_fn_t fn, void* arg) {
    /* 快路径：已完成 */
    if (atomic_load_explicit(&o->word, memory_order_acquire) == 2) return;
    /* 抢执行权 */
    uint32_t exp = 0;
    if (atomic_compare_exchange_strong_explicit(&o->word, &exp, 1,
            memory_order_acquire, memory_order_relaxed)) {
        fn(arg);
        atomic_store_explicit(&o->word, 2, memory_order_release);
        lm_butex_wake_all(&o->word);
        return;
    }
    /* 未抢到：等执行者完成（字变 2） */
    for (;;) {
        uint32_t v = atomic_load_explicit(&o->word, memory_order_acquire);
        if (v == 2) return;
        lm_butex_wait(&o->word, v);
    }
}
