// lm_stack_pool.c —— Phase 8.6 B：per-thread 栈池实现
// 借鉴 bthread StackFactory（brpc stack_inl.h:124-161）+ stack.cpp:56-131 allocate_stack_storage。
//
// intrusive free list：归还的栈，可用区首 word 存 next 指针（in-band），
// 无额外节点 malloc。get 时读 next、出栈；return 时写 next、入栈。
// ASAN：return poison 整个可用区，get unpoison，与 bthread stack_inl.h:151-158 一致。
#include "lm_stack_pool.h"
#include "lm_co.h"          /* LM_STACK_SMALL/NORMAL/CLASS_* 常量 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

/* ============================================================
 * ASAN poison/unpoison（仅 AddressSanitizer 构建生效，否则空操作）
 * 对齐 bthread stack_inl.h:34-94 的 __asan_poison/unpoison 配对。
 * 池中空闲栈标记为不可访问，防 co 代码持有栈变量悬空引用误用。
 * ============================================================ */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LM_ASAN_POOL 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LM_ASAN_POOL)
#  define LM_ASAN_POOL 1
#endif
#ifdef LM_ASAN_POOL
/* poison/unpoison 声明在 asan_interface.h（common_interface_defs.h 仅含
 * start/finish_switch_fiber 等 fiber API，不含 poison）。对齐 bthread
 * stack_inl.h:34-94 的 poison/unpoison 配对语义。 */
#include <sanitizer/asan_interface.h>
#define LM_POOL_POISON(p, n)   __asan_poison_memory_region((p), (n))
#define LM_POOL_UNPOISON(p, n) __asan_unpoison_memory_region((p), (n))
#else
#define LM_POOL_POISON(p, n)   ((void)0)
#define LM_POOL_UNPOISON(p, n) ((void)0)
#endif

/* 池容量（对齐 bthread stack.cpp:38-39 tc_stack_small=32 / tc_stack_normal=8）。 */
#define LM_POOL_SMALL_CAP   32
#define LM_POOL_NORMAL_CAP  8

/* per-thread 池状态：每桶一个 free list + count。
 * free list 节点 in-band：可用区首 word = next 指针。
 *
 * 注意：池结构必须堆分配（calloc），不能是 _Thread_local 静态变量。
 * macOS 线程退出时动态 TLS 块先于 pthread_key 析构释放，若 setspecific 存
 * TLS 地址，dtor 解引用即 use-after-free（ASAN 全量回归实测）。
 * 堆分配的池由 key 析构 free，生命周期完全自管。 */
typedef struct {
    void* small_free;    /* free list head（mmap_base），NULL=空 */
    int   small_count;
    void* normal_free;
    int   normal_count;
} lm_pool_tls_t;

/* 线程退出析构：munmap 拋留池中栈并 free 池结构，防泄漏。
 * 进程 exit() 不保证调（OS 回收全部内存，无需）。 */
static pthread_key_t g_pool_key;
static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;

/* 清空池：unpoison 后读 in-band next 逐个 munmap，最后 free 池结构。
 * dtor（线程退出）与 atexit（主线程 exit）两条路径共用。 */
static void pool_drain_and_free(lm_pool_tls_t* pool) {
    if (!pool) return;
    /* munmap small free list */
    void* p = pool->small_free;
    while (p) {
        LM_POOL_UNPOISON(p, LM_STACK_SMALL);   /* 归还时已 poison，读 next 前清除 */
        void* next = *(void**)p;
        munmap(p, LM_STACK_SMALL + 4096);      /* guard page 按 4KiB（get 时已对齐） */
        p = next;
    }
    p = pool->normal_free;
    while (p) {
        LM_POOL_UNPOISON(p, LM_STACK_NORMAL);
        void* next = *(void**)p;
        munmap(p, LM_STACK_NORMAL + 4096);
        p = next;
    }
    free(pool);
}

static void pool_thread_dtor(void* arg) {
    pool_drain_and_free((lm_pool_tls_t*)arg);
}

/* 主线程 exit() 路径不触发 key 析构，atexit 兜底 free 池结构（32 字节）。
 * 若 dtor 已先行（pthread_exit 路径），specific 已被清空，此处跳过。 */
static void pool_main_atexit(void) {
    lm_pool_tls_t* pool = (lm_pool_tls_t*)pthread_getspecific(g_pool_key);
    if (pool) {
        pool_drain_and_free(pool);
        pthread_setspecific(g_pool_key, NULL);
    }
}

static void pool_key_init(void) {
    pthread_key_create(&g_pool_key, pool_thread_dtor);
    atexit(pool_main_atexit);
}

/* 取当前线程池：首次使用时 calloc 并挂 key（key 槽在 pthread 内部 TSD 数组，
 * 生命周期覆盖 dtor 运行期，dtor 拿到的指针始终有效）。
 * 返回 NULL 仅在 calloc 失败时发生。 */
static lm_pool_tls_t* pool_for_thread(void) {
    pthread_once(&g_pool_once, pool_key_init);
    lm_pool_tls_t* pool = (lm_pool_tls_t*)pthread_getspecific(g_pool_key);
    if (!pool) {
        pool = (lm_pool_tls_t*)calloc(1, sizeof(*pool));
        if (!pool) return NULL;
        pthread_setspecific(g_pool_key, pool);
    }
    return pool;
}

/* ============================================================
 * 栈分配：mmap + 末页 guard page（对齐 bthread stack.cpp:56-131）
 * 逻辑与 lm_co.c:co_alloc_stack 一致，迁出供池使用。
 * 返回 0 成功填充 *out；-1 失败。
 * ============================================================ */
static int pool_alloc_stack(size_t stack_size, lm_stack_storage_t* out) {
    long page_l = sysconf(_SC_PAGESIZE);
    if (page_l <= 0) page_l = 4096;
    size_t page = (size_t)page_l;
    if (stack_size < page) stack_size = page;
    stack_size = (stack_size + page - 1) & ~(page - 1);   /* 页对齐 */
    size_t total = stack_size + page;
    void* p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return -1;
    /* 末页（高地址方向：p+stack_size 到 p+stack_size+page）guard */
    if (mprotect((char*)p + stack_size, page, PROT_NONE) != 0) {
        munmap(p, total);
        return -1;
    }
    out->mmap_base  = p;
    out->stack_size = stack_size;
    out->stack_base = (char*)p + stack_size;   /* 高地址，可用区末 */
    out->stack_top  = p;                        /* 低地址，可用区起 */
    out->total      = total;
    /* 新分配的栈直接交给协程用，不 poison（fresh mmap 默认 unpoisoned）。 */
    return 0;
}

/* ============================================================
 * API
 * ============================================================ */

int lm_stack_pool_get(int stack_class, lm_stack_storage_t* out) {
    lm_pool_tls_t* pool = pool_for_thread();
    if (!pool) return -1;
    void** free_head;
    int* count;
    size_t want_size;
    if (stack_class == LM_STACK_CLASS_SMALL) {
        free_head = &pool->small_free;
        count = &pool->small_count;
        want_size = LM_STACK_SMALL;
    } else {
        free_head = &pool->normal_free;
        count = &pool->normal_count;
        want_size = LM_STACK_NORMAL;
    }
    /* 池命中：unpoison + 出栈。 */
    if (*free_head) {
        void* p = *free_head;
        LM_POOL_UNPOISON(p, want_size);   /* 恢复可访问 */
        void* next = *(void**)p;           /* 读 in-band next */
        *free_head = next;
        (*count)--;
        out->mmap_base  = p;
        out->stack_size = want_size;
        out->stack_base = (char*)p + want_size;
        out->stack_top  = p;
        long page_l = sysconf(_SC_PAGESIZE);
        if (page_l <= 0) page_l = 4096;
        out->total = want_size + (size_t)page_l;
        return 0;
    }
    /* 池空：分配新栈。 */
    return pool_alloc_stack(want_size, out);
}

void lm_stack_pool_return(const lm_stack_storage_t* st) {
    if (!st || !st->mmap_base) return;
    lm_pool_tls_t* pool = pool_for_thread();
    void** free_head;
    int* count;
    int cap;
    size_t want_size;
    if (st->stack_size <= LM_STACK_SMALL) {
        free_head = pool ? &pool->small_free : NULL;
        count = pool ? &pool->small_count : NULL;
        cap = LM_POOL_SMALL_CAP;
        want_size = LM_STACK_SMALL;
    } else {
        free_head = pool ? &pool->normal_free : NULL;
        count = pool ? &pool->normal_count : NULL;
        cap = LM_POOL_NORMAL_CAP;
        want_size = LM_STACK_NORMAL;
    }
    /* 归还的栈大小可能与桶档不完全匹配（用户指定了非标准 size），
     * 此时直接 munmap 不入池（池只缓存标准档栈，保证 get 命中尺寸一致）。 */
    if (!pool || st->stack_size != want_size || *count >= cap) {
        /* 池不可用 / 尺寸不匹配 / 池满：munmap 归还 OS。 */
        munmap(st->mmap_base, st->total);
        return;
    }
    /* 入栈：先写 in-band next（此刻可用区仍 unpoison），再 poison 整区。 */
    *(void**)st->mmap_base = *free_head;
    LM_POOL_POISON(st->mmap_base, want_size);
    *free_head = st->mmap_base;
    (*count)++;
}

int lm_stack_pool_count(int stack_class) {
    /* 只读接口：无池不创建（返回 0）。 */
    pthread_once(&g_pool_once, pool_key_init);
    lm_pool_tls_t* pool = (lm_pool_tls_t*)pthread_getspecific(g_pool_key);
    if (!pool) return 0;
    if (stack_class == LM_STACK_CLASS_SMALL) return pool->small_count;
    return pool->normal_count;
}
