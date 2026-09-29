// lm_butex.c —— 统一 futex 级阻塞原语实现（Phase 8.3）
// addr 哈希分桶 waiter 表 + 平台 futex 分发。语义与契约见 lm_butex.h。
#include "lm_butex.h"
#include "lm_co.h"
#include "lm_scheduler.h"

#include <stdlib.h>
#include <errno.h>
#include <pthread.h>

/* ============================================================
 * 平台 futex 封装
 *   Linux：SYS_futex（FUTEX_WAIT/WAKE_PRIVATE）
 *   macOS：__ulock_wait/__ulock_wake（私有 syscall，UL_COMPARE_AND_WAIT=1/
 *          UL_COMPARE_AND_WAIT_WAKE=2；Chromium/Rust std 生产使用）
 * 两者语义一致：仅当 *word == expected 时内核态挂起；wake 唤醒一个。
 * ============================================================ */
#ifdef __linux__
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>

/* timeout_us：<0 无限等待（pts=NULL），>=0 超时。Phase 8.4 起为
 * lm_futex_wait 提供超时能力（timer 线程睡到最近到期点）。 */
static long platform_park_raw(volatile _Atomic uint32_t* word, uint32_t expected,
                              int64_t timeout_us) {
    struct timespec ts, *pts = NULL;
    if (timeout_us >= 0) {
        ts.tv_sec = (time_t)(timeout_us / 1000000);
        ts.tv_nsec = (long)(timeout_us % 1000000) * 1000;
        pts = &ts;
    }
    return syscall(SYS_futex, (const uint32_t*)word, FUTEX_WAIT_PRIVATE,
                   expected, pts, NULL, 0);
}

static long platform_wake_raw(volatile _Atomic uint32_t* word) {
    return syscall(SYS_futex, (const uint32_t*)word, FUTEX_WAKE_PRIVATE,
                   1, NULL, NULL, 0);
}
#else  /* macOS（其他 BSD 同样走此路径） */
/* xnu 私有头 bsd/sys/ulock.h（公共 SDK 不提供）：
 *   UL_COMPARE_AND_WAIT = 1：wait/wake 共用此操作码——wait 时内核比较
 *     *addr == value 才挂起；wake 时 flags=0 唤醒一个等待者。
 *   ULF_WAKE_ALL = 0x100：wake 标志位（与操作码按位或）。
 *   注意：操作码 2 是 UL_UNFAIR_LOCK，不是 compare-and-wait 的 wake——
 *   误用会以 EDOM 失败（实测定案）。 */
#define UL_COMPARE_AND_WAIT       1
#define ULF_WAKE_ALL              0x00000100

/* 私有接口原型（公共 SDK 头不提供，自行声明；符号经 libSystem 导出） */
extern int __ulock_wait(uint32_t operation, void* addr, uint64_t value,
                        uint32_t timeout);
extern int __ulock_wake(uint32_t operation, void* addr, uint64_t wakeValue);

static long platform_park_raw(volatile _Atomic uint32_t* word, uint32_t expected,
                              int64_t timeout_us) {
    /* __ulock_wait 第 4 参 timeout 单位为微秒（xnu sys_ulock.c 按微秒换算），
     * 0 表示无限等待。实测传毫秒会 1000 倍过短（99ms→99µs），timer 线程
     * 空转 ~16kHz（compute_test 挂死排查定位）。直接透传微秒并截断 uint32。 */
    uint32_t timeout = 0;
    if (timeout_us >= 0) {
        timeout = (timeout_us > 0xFFFFFFFFLL) ? 0xFFFFFFFFu : (uint32_t)timeout_us;
    }
    return __ulock_wait(UL_COMPARE_AND_WAIT, (void*)word, (uint64_t)expected, timeout);
}

static long platform_wake_raw(volatile _Atomic uint32_t* word) {
    /* wake 与 wait 同操作码，flags 0 = 唤醒一个 */
    return __ulock_wake(UL_COMPARE_AND_WAIT, (void*)word, 0);
}
#endif

/* 平台 syscall 可用性：-1 未探测 / 1 可用 / 0 ENOSYS 走 pthread cond 保底。
 * 探测用"必不匹配"的期望值——syscall 存在则立即返回，不会真睡。 */
static int g_platformOk = -1;

static int platform_available(void) {
    if (g_platformOk >= 0) return g_platformOk;
    _Atomic uint32_t probe = 0;
    long r = platform_park_raw(&probe, 1, -1);
    /* Linux 值不匹配返回 -1/EAGAIN；macOS 返回 0 或 -1（值变语义）。
     * 仅 ENOSYS 判定 syscall 不存在。 */
    if (r < 0 && errno == ENOSYS) {
        g_platformOk = 0;
    } else {
        g_platformOk = 1;
    }
    return g_platformOk;
}

/* ============================================================
 * waiter 表：按 addr 哈希分桶，桶内单链，每桶一把 mutex。
 * 等待者入队 / 唤醒者摘队都经桶锁串行——同 addr 的 wait/wake 顺序
 * 由该锁 + 字值 recheck 共同保证（丢唤醒窗口关闭，见 wait 内注释）。
 * ============================================================ */
#define BUTEX_BUCKETS 256   /* 必须为 2 的幂 */

typedef struct ButexEntry {
    volatile _Atomic uint32_t* key;  /* 等待的原子字地址 */
    lm_co_t* co;                     /* 非 NULL = 协程等待者；NULL = 线程等待者 */
    lm_scheduler_t* sched;           /* 协程所属 scheduler（唤醒投递目标） */
    _Atomic uint32_t notified;       /* 线程等待者的自有 park 字（0/1） */
    int fbInited;                    /* pthread cond 保底是否已初始化 */
    pthread_mutex_t fbMutex;
    pthread_cond_t fbCond;
    _Atomic int refcnt;              /* 引用计数：wait 持 1 + wake 摘队持 1，
                                      * 双方 release，ref→0 时 destroy。
                                      * 修复 ASAN UAF：wake 通知阶段读 entry 与
                                      * 线程醒来 destroy 并发的竞态。 */
    struct ButexEntry* next;
} butex_entry_t;

typedef struct {
    pthread_mutex_t lock;
    butex_entry_t* head;
} butex_bucket_t;

static butex_bucket_t g_buckets[BUTEX_BUCKETS];

__attribute__((constructor))
static void butex_ctor(void) {
    for (int i = 0; i < BUTEX_BUCKETS; i++) {
        pthread_mutex_init(&g_buckets[i].lock, NULL);
        g_buckets[i].head = NULL;
    }
}

/* 原子字 4 字节对齐，右移 2 位取哈希 */
static inline unsigned bucket_index(volatile _Atomic uint32_t* addr) {
    return (unsigned)(((uintptr_t)addr >> 2) & (BUTEX_BUCKETS - 1));
}

static void entry_destroy(butex_entry_t* e) {
    if (e->fbInited) {
        pthread_mutex_destroy(&e->fbMutex);
        pthread_cond_destroy(&e->fbCond);
    }
    free(e);
}

/* 引用计数 release：ref→0 时 destroy。
 * wait 创建持 1，wake 摘队 +1（持有 picked 引用），wake 通知完 release，
 * wait 退出 release。两路并发 release，最后者 destroy——修复 wake 通知
 * 阶段读 entry 与线程醒来 destroy 的 UAF 竞态。 */
static void entry_release(butex_entry_t* e) {
    if (atomic_fetch_sub_explicit(&e->refcnt, 1, memory_order_acq_rel) == 1) {
        entry_destroy(e);
    }
}

/* ---- S4 统计 ---- */
static _Atomic long g_emptyWakes = 0;
static _Atomic long g_parkSyscalls = 0;

long lm_butex_empty_wakes(void) {
    return atomic_load_explicit(&g_emptyWakes, memory_order_acquire);
}

long lm_butex_park_syscalls(void) {
    return atomic_load_explicit(&g_parkSyscalls, memory_order_acquire);
}

/* ============================================================
 * wait
 * ============================================================ */
int lm_butex_wait(volatile _Atomic uint32_t* addr, uint32_t expected) {
    if (!addr) return 0;
    /* 第一次值比较：值已变更则不占任何资源直接返回 */
    if (atomic_load_explicit(addr, memory_order_acquire) != expected) return 0;

    lm_co_t* co = lm_co_current();
    lm_scheduler_t* sched = NULL;
    if (co) {
        sched = lm_scheduler_get_current();
        /* 跨线程唤醒须有投递目标：无 scheduler 的协程不入睡（契约见头文件） */
        if (!sched) return 0;
    }

    butex_entry_t* e = (butex_entry_t*)calloc(1, sizeof(butex_entry_t));
    if (!e) return 0;
    e->key = addr;
    e->co = co;
    e->sched = sched;
    atomic_init(&e->notified, 0);
    atomic_init(&e->refcnt, 1);   /* wait 持 1；wake 摘队 +1，通知完 release */

    /* 线程等待者且平台 futex 不可用：准备 pthread cond 保底 */
    int useFallback = (!co && !platform_available());
    if (useFallback) {
        if (pthread_mutex_init(&e->fbMutex, NULL) != 0 ||
            pthread_cond_init(&e->fbCond, NULL) != 0) {
            entry_destroy(e);
            return 0;
        }
        e->fbInited = 1;
    }

    unsigned idx = bucket_index(addr);
    pthread_mutex_lock(&g_buckets[idx].lock);
    /* 锁内 recheck（丢唤醒防护核心）：唤醒方协议为"先改字、后 wake"，
     * wake 摘队同样要拿本桶锁。二者交错只可能是：
     *   - 改字先于本 recheck → 看到新值，不入睡；
     *   - wake 先拿锁 → 本 entry 尚未入队，唤醒方找不到（空唤醒），
     *     但改字必已发生 → 本 recheck 看到新值，不入睡；
     *   - 本 entry 入队后 → 唤醒方必能在桶内看到并摘除。 */
    if (atomic_load_explicit(addr, memory_order_acquire) != expected) {
        pthread_mutex_unlock(&g_buckets[idx].lock);
        entry_destroy(e);
        return 0;
    }
    e->next = g_buckets[idx].head;
    g_buckets[idx].head = e;
    pthread_mutex_unlock(&g_buckets[idx].lock);

    if (co) {
        /* 协程等待：yield 切回本线程 scheduler。
         * 安全性说明：唤醒方定向 post 回本协程【自己的】scheduler，消费该队列
         * 的只可能是当前这条线程（scheduler owner）在本协程 yield 之后——
         * 不会有其他线程提前 resume 造成双线程同栈。entry 已由唤醒方摘队。 */
        lm_co_yield();
        entry_release(e);   /* wake 摘队时 +ref，通知完已 release；此处 drop wait 引用 */
        return 1;
    }

    /* 线程等待：在 entry 自有的 park 字上阻塞。
     * 唤醒方先置 notified=1 再 platform_wake：若 wake 先于本线程进入 syscall，
     * 值不匹配（Linux EAGAIN / macOS 值变返回），recheck 后直接退出。 */
    if (useFallback) {
        pthread_mutex_lock(&e->fbMutex);
        while (atomic_load_explicit(&e->notified, memory_order_acquire) == 0) {
            pthread_cond_wait(&e->fbCond, &e->fbMutex);
        }
        pthread_mutex_unlock(&e->fbMutex);
    } else {
        atomic_fetch_add_explicit(&g_parkSyscalls, 1, memory_order_relaxed);
        while (atomic_load_explicit(&e->notified, memory_order_acquire) == 0) {
            long r = platform_park_raw(&e->notified, 0, -1);
            if (r < 0 && errno == ENOSYS) break;  /* 保险：不应发生 */
            /* EINTR / EAGAIN / macOS 值变：recheck notified 后决定去留 */
        }
    }
    entry_release(e);   /* drop wait 引用；wake 若已摘队+通知完则此处 destroy */
    return 1;
}

/* ============================================================
 * wake 内部实现：摘除至多 maxCount 个同 key entry，桶锁外逐个通知。
 * ============================================================ */
static int butex_wake_internal(volatile _Atomic uint32_t* addr, int maxCount) {
    if (!addr || maxCount <= 0) return 0;
    unsigned idx = bucket_index(addr);
    butex_entry_t* picked = NULL;
    int count = 0;

    pthread_mutex_lock(&g_buckets[idx].lock);
    butex_entry_t** pp = &g_buckets[idx].head;
    while (*pp && count < maxCount) {
        if ((*pp)->key == addr) {
            butex_entry_t* e = *pp;
            *pp = e->next;          /* 摘队 */
            e->next = picked;
            picked = e;
            /* +ref：wake 持 picked 引用，使通知阶段读 entry 时 wait 侧
             * release 不会先 destroy（修复 UAF）。通知完 entry_release。 */
            atomic_fetch_add_explicit(&e->refcnt, 1, memory_order_acq_rel);
            count++;
        } else {
            pp = &(*pp)->next;
        }
    }
    pthread_mutex_unlock(&g_buckets[idx].lock);

    if (count == 0) {
        /* S4：无 waiter，全程无任何系统调用 */
        atomic_fetch_add_explicit(&g_emptyWakes, 1, memory_order_relaxed);
        return 0;
    }

    /* 通知阶段放在桶锁外：post/syscall 不阻塞同桶其他 wait/wake。
     * 先存 next 再 release——release 可能 destroy e 使 e->next 失效。 */
    butex_entry_t* e = picked;
    while (e) {
        butex_entry_t* next = e->next;
        if (e->co) {
            /* 定向唤醒：post 回协程所属 scheduler（pinned 定向语义）。
             * sched 非空由 wait 侧保证。 */
            lm_scheduler_wakeup(e->sched, e->co);
        } else {
            atomic_store_explicit(&e->notified, 1, memory_order_release);
            if (e->fbInited) {
                pthread_mutex_lock(&e->fbMutex);
                pthread_cond_signal(&e->fbCond);
                pthread_mutex_unlock(&e->fbMutex);
            } else {
                platform_wake_raw(&e->notified);
            }
        }
        entry_release(e);   /* drop wake 引用；wait 侧 release 时最后者 destroy */
        e = next;
    }
    return count;
}

int lm_butex_wake(volatile _Atomic uint32_t* addr, int maxCount) {
    return butex_wake_internal(addr, maxCount);
}

int lm_butex_wake_all(volatile _Atomic uint32_t* addr) {
    return butex_wake_internal(addr, 0x7fffffff);
}

/* ============================================================
 * Phase 8.4：裸 futex 等待/唤醒（timer 线程专用）
 * 见 lm_butex.h 头部契约。仅服务单等待者场景（timer 线程私有 _nsignals），
 * 不经 entry 表：直接平台 syscall，无分配/摘链开销。
 * ============================================================ */
int lm_futex_wait(volatile _Atomic uint32_t* word, uint32_t expected, int64_t timeout_us) {
    if (!word) return 0;
    /* 值已变更则不睡（与 lm_butex_wait 同语义，关闭丢唤醒） */
    if (atomic_load_explicit(word, memory_order_acquire) != expected) return 0;
    if (!platform_available()) return 0;
    atomic_fetch_add_explicit(&g_parkSyscalls, 1, memory_order_relaxed);
    long r = platform_park_raw(word, expected, timeout_us);
    if (r >= 0) return 1;   /* 被唤醒（macOS __ulock_wait 成功返回 0） */
    /* r < 0：值变更（EAGAIN）/超时（ETIMEDOUT）/中断（EINTR）。
     * 统一返回 0，caller recheck 值与时间决定去留——对齐 timer_thread
     * futex_wait 返回后由 run 循环重新计算最近到期点的语义。 */
    return 0;
}

int lm_futex_wake(volatile _Atomic uint32_t* word) {
    if (!word) return 0;
    if (!platform_available()) return 0;
    return (int)platform_wake_raw(word);
}
