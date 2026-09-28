// lm_co.c —— 有栈协程实现（POSIX ucontext）
// 参考 libco/libhv withloop 思路：swapcontext 切换栈，yield 时整条 C 调用栈冻结。
// 协程内调 lm_co_yield 切回 resume 调用方（reactor 主循环）；
// reactor 调度回来 lm_co_resume 从 yield 点继续。
//
// 协程指针通过 makecontext 的 2 个 unsigned int 参数传递（拼 64 位指针），
// 避免 g_startup_co 全局变量的多协程 spawn 竞态（spawn A → spawn B 覆盖 → resume A 读错）。
//
// 栈分配：mmap size + page，末页（高地址方向）mprotect PROT_NONE 作 guard page，
// 爆栈时触发 SIGSEGV 而非静默破坏内存。
#include "lm_co.h"
#include "gc_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <pthread.h>

/* ============================================================
 * ASAN fiber 注解（仅 AddressSanitizer 构建生效，否则为空操作）
 * ucontext swapcontext 切换栈对 ASAN 不可见：协程栈上的合法访问被误判为
 * stack-use-after-scope（google/sanitizers#189 假阳性）。按官方 fiber API
 * 在三个切换点 bookkeeping 告知 ASAN 栈归属：
 *   resume/yield 切走前 __sanitizer_start_switch_fiber（存当前流 fake +
 *   声明目标栈区间）；切入后 __sanitizer_finish_switch_fiber（恢复本流 fake）。
 * entry 经 uc_link 隐式切回时，trampoline 返回前补 start_switch 配对。 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LM_ASAN_FIBER 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LM_ASAN_FIBER)
#  define LM_ASAN_FIBER 1
#endif
/* 对照开关：-DLM_ASAN_FIBER_OFF 强制关闭注解（排查 ASAN 行为差异用） */
#ifdef LM_ASAN_FIBER_OFF
#  undef LM_ASAN_FIBER
#endif
#ifdef LM_ASAN_FIBER
#include <sanitizer/common_interface_defs.h>
/* 主/线程栈的 fake 槽与栈区间（每线程一份，惰性缓存） */
static _Thread_local void*  tl_main_fake = NULL;
static _Thread_local void*  tl_main_bottom = NULL;
static _Thread_local size_t tl_main_size = 0;
static void asan_main_stack_init(void) {
    if (tl_main_size) return;
#ifdef __APPLE__
    /* macOS：stackaddr 返回栈基址（高地址），bottom = base - size */
    void* base = pthread_get_stackaddr_np(pthread_self());
    size_t sz = pthread_get_stacksize_np(pthread_self());
    tl_main_bottom = (char*)base - sz;
    tl_main_size = sz;
#else
    pthread_attr_t attr;
    void* base = NULL;
    size_t sz = 0;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        pthread_attr_getstack(&attr, &base, &sz);
        pthread_attr_destroy(&attr);
    }
    tl_main_bottom = base;
    tl_main_size = sz;
#endif
}
#endif /* LM_ASAN_FIBER */

/* ============================================================
 * TLS：当前协程指针
 * ============================================================ */

static pthread_key_t g_co_tls_key;
static pthread_once_t g_co_tls_once = PTHREAD_ONCE_INIT;

static void co_tls_init(void) {
    pthread_key_create(&g_co_tls_key, NULL);
}

lm_co_t* lm_co_current(void) {
    pthread_once(&g_co_tls_once, co_tls_init);
    return (lm_co_t*)pthread_getspecific(g_co_tls_key);
}

static void co_set_current(lm_co_t* co) {
    pthread_once(&g_co_tls_once, co_tls_init);
    pthread_setspecific(g_co_tls_key, co);
}

/* ============================================================
 * Phase 5: VM 状态保存/恢复 hook
 * 协程在同线程复用 _Thread_local 的 g_stack_mgr + 异常栈（g_try_stack 等），
 * yield/resume 必须保存/恢复这组状态。vm 层注册 hook 读写 co->vm_state。
 * ============================================================ */
static lm_co_vm_hook_t g_co_resume_hook = NULL;  /* swapcontext 前恢复协程 vm_state */
static lm_co_vm_hook_t g_co_yield_hook = NULL;   /* swapcontext 前保存协程 vm_state + 恢复基线 */
static lm_co_vm_hook_t g_co_release_hook = NULL;  /* destroy 前释放 vm_state 内存 */

void lm_co_set_vm_hooks(lm_co_vm_hook_t on_resume, lm_co_vm_hook_t on_yield,
                        lm_co_vm_hook_t on_release) {
    g_co_resume_hook = on_resume;
    g_co_yield_hook = on_yield;
    g_co_release_hook = on_release;
}

/* ============================================================
 * 协程 trampoline：makecontext 入口
 * 接收 2 个 unsigned int 拼成 64 位指针（避免全局变量竞态）。
 * entry 返回后由 ucontext 的 uc_link 机制自动 swapcontext 回 resume_ctx。
 * ============================================================ */

static void co_trampoline(unsigned int hi, unsigned int lo) {
    uintptr_t p = ((uintptr_t)hi << 32) | (uintptr_t)lo;
    lm_co_t* co = (lm_co_t*)p;
#ifdef LM_ASAN_FIBER
    /* 首次切入协程：resume 侧 start_switch 已声明本栈，恢复本协程 fake（初始 NULL） */
    __sanitizer_finish_switch_fiber(co->asan_fake, NULL, NULL);
#endif
    /* 进入协程：设 TLS，调 entry */
    co_set_current(co);
    co->state = LM_CO_RUNNING;
    if (co->entry) {
        co->entry(co->arg);
    }
    /* entry 返回：协程结束，转 DEAD，清 TLS。
     * uc_link 已设为 &resume_ctx，函数返回后自动 swapcontext 切回。 */
    co->state = LM_CO_DEAD;
    co_set_current(NULL);
#ifdef LM_ASAN_FIBER
    /* 函数返回经 uc_link 隐式 swapcontext 回 resume_ctx：补配对的 start_switch
     * （存本协程 fake，声明切回目标 caller 栈） */
    __sanitizer_start_switch_fiber(&co->asan_fake, co->asan_caller_bottom,
                                   co->asan_caller_size);
#endif
    /* 函数返回 → ucontext 自动 swapcontext(&co->ctx->uc_link=&resume_ctx) */
}

/* ============================================================
 * 栈分配：mmap + 末页 guard page
 * ============================================================ */

/* 分配栈：返回可用区末（高地址，栈基址）；*out_mmap 返回 mmap 起点（低地址，用于 munmap）。
 * 栈从高地址向低地址生长，所以末页（高地址方向）guard，可用区为 [ptr, ptr+stack_size)。
 * 但 mmap 返回的 ptr 是低地址，所以 guard page 在 ptr+stack_size 处。 */
static void* co_alloc_stack(size_t stack_size, void** out_mmap, size_t* out_total) {
    long page_l = sysconf(_SC_PAGESIZE);
    if (page_l <= 0) page_l = 4096;
    size_t page = (size_t)page_l;
    if (stack_size < page) stack_size = page;
    /* 页对齐 stack_size */
    stack_size = (stack_size + page - 1) & ~(page - 1);
    /* mmap size + page（末页 guard） */
    size_t total = stack_size + page;
    void* p = mmap(NULL, total, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return NULL;
    /* 末页（高地址方向：p+stack_size 到 p+stack_size+page）guard */
    if (mprotect((char*)p + stack_size, page, PROT_NONE) != 0) {
        munmap(p, total);
        return NULL;
    }
    *out_mmap = p;
    *out_total = total;
    /* 栈基址 = 可用区末（高地址），ucontext 用此作为 ss_sp 起点（自管生长方向） */
    return (char*)p + stack_size;
}

/* ============================================================
 * API
 * ============================================================ */

lm_co_t* lm_co_spawn(lm_co_entry_t entry, void* arg, size_t stack_size) {
    if (stack_size == 0) stack_size = LM_CO_DEFAULT_STACK_SIZE;
    lm_co_t* co = (lm_co_t*)calloc(1, sizeof(lm_co_t));
    if (!co) return NULL;
    void* mmap_base = NULL;
    size_t total = 0;
    void* stack_base = co_alloc_stack(stack_size, &mmap_base, &total);
    if (!stack_base) {
        free(co);
        return NULL;
    }
    co->stack_mmap = mmap_base;   /* 低地址，munmap 用 */
    co->stack_size = stack_size;
    co->stack_base = stack_base; /* 高地址，栈基址 */
    co->stack_top = mmap_base;   /* 低地址，可用区起 */
    co->entry = entry;
    co->arg = arg;
    co->state = LM_CO_READY;
    co->next = NULL;

    /* makecontext：ucontext_t 的 uc_stack.ss_sp 设为栈基址（高地址），
     * 实际栈从 ss_sp 向下生长；ss_size 为可用区大小。
     * 注意：不同平台对 ss_sp 语义略异，POSIX 规定 ss_sp 为最低地址（可用区起），
     * 但 glibc 与 macOS 实测：ss_sp 设为栈基址（高地址）+ ss_size，ucontext 自管生长。
     * 兼容写法：ss_sp = mmap_base（可用区起），ss_size = stack_size。 */
    getcontext(&co->ctx);
    co->ctx.uc_stack.ss_sp = mmap_base;
    co->ctx.uc_stack.ss_size = stack_size;
    co->ctx.uc_link = &co->resume_ctx;   /* entry 返回后自动切回 resume_ctx */
    /* 拆 64 位指针为 2 个 unsigned int 传给 trampoline */
    uintptr_t p = (uintptr_t)co;
    unsigned int hi = (unsigned int)(p >> 32);
    unsigned int lo = (unsigned int)(p & 0xFFFFFFFFu);
    makecontext(&co->ctx, (void(*)(void))co_trampoline, 2, hi, lo);
    return co;
}

void lm_co_resume(lm_co_t* co) {
    if (!co || co->state == LM_CO_DEAD) return;
    /* Phase 7.2：scheduler 投递在 SPAWN 时做（BUILTIN_CO_SPAWN 检查
     * sched->current==NULL 时 post 到就绪队列），resume 走纯直连 swapcontext
     * 路径——reactor drain_ready 钩子直接调本函数消费就绪队列。
     * 协程内 spawn（current!=NULL）不投递，由 spawning 协程显式 resume。 */
    /* 保存当前（resume 调用方）上下文到 co->resume_ctx，切到 co->ctx。
     * caller 可能是 NULL（reactor 主循环）或另一个协程（协程内嵌套 resume，
     * 如 server_co accept 后 spawn echo_co 并 resume 启动）。 */
    lm_co_t* caller = lm_co_current();
    lm_co_state_t prev = co->state;
    co->state = LM_CO_RUNNING;
    co_set_current(co);
    /* Phase 5: swapcontext 切到协程前，恢复协程 vm_state 到 _Thread_local
     * （g_stack_mgr sp / g_try_stack / g_unwind / g_err_jmp / g_thread_root 等）。
     * 首次 resume 时 co->vm_state 由 spawn 方预分配为全 0（协程从空状态开始）。 */
    if (g_co_resume_hook) g_co_resume_hook(co);
#ifdef LM_ASAN_FIBER
    /* 记录 yield/结束切回的目标栈（caller 协程栈或当前线程主栈），
     * 供协程侧 start_switch 声明；随后 start_switch：存 caller fake，
     * 声明进入 co 栈（stack_top = 可用区低地址 bottom）。 */
    if (caller) {
        co->asan_caller_bottom = caller->stack_top;
        co->asan_caller_size = caller->stack_size;
    } else {
        asan_main_stack_init();
        co->asan_caller_bottom = tl_main_bottom;
        co->asan_caller_size = tl_main_size;
    }
    __sanitizer_start_switch_fiber(caller ? &caller->asan_fake : &tl_main_fake,
                                   co->stack_top, co->stack_size);
#endif
    swapcontext(&co->resume_ctx, &co->ctx);
#ifdef LM_ASAN_FIBER
    /* 协程 yield/结束切回这里：恢复 caller 自己的 fake */
    __sanitizer_finish_switch_fiber(caller ? caller->asan_fake : tl_main_fake,
                                    NULL, NULL);
#endif
    /* 协程 yield 或 entry 返回后切回这里。
     * 协程 yield 时 TLS 已被 yield 清为 NULL（切回前）且 vm_state 已保存 + baseline 已恢复；
     * entry 返回时 trampoline 已清 TLS，但 _Thread_local 仍是协程最终状态（DEAD 场景），
     * 这里调 yield_hook(co) 恢复 baseline（co->state==DEAD 时跳过保存，只恢复 baseline）。 */
    if (co->state == LM_CO_DEAD && g_co_yield_hook) g_co_yield_hook(co);
    /* 恢复 caller 的 current：NULL=reactor 主循环无协程上下文；非 NULL=外层协程
     * （嵌套 resume 场景，外层协程继续执行时 lm_co_current() 需返回外层 co，
     * 否则后续 in_co 判定失效走阻塞分支）。 */
    co_set_current(caller);
    (void)prev;
}

void lm_co_yield(void) {
    lm_co_t* co = lm_co_current();
    if (!co || co->state != LM_CO_RUNNING) return;
    co->state = LM_CO_SUSPENDED;
    /* yield 前注册冻结栈区间，供 GC 保守扫描（否则 yield 期间 GC 漏标
     * C 局部 Value 变量 → sweep 误回收 → UAF）。
     * stack_top=可用区最高 word（高地址），stack_bottom=当前帧地址（低地址）。
     * 注意：co->stack_base 指向 guard page 起点（mmap 区末，PROT_NONE 不可读），
     * 必须下调一个 word 到可用区内，否则保守扫描读 *stack_top 触发 SIGBUS。
     * 不调 gc_enter_native_block：swapcontext 返回后 reactor 主循环继续运行并可能
     * 变更 GC 根，at_safepoint=1 会误报安全点 → 并发 GC 扫描期间根被修改 → UAF。
     * STW 轮询由 reactor 主循环每轮 gc_stw_check_fast() 承担。 */
    void* curFrame = __builtin_frame_address(0);
    void* scanTop = (char*)co->stack_base - sizeof(void*);
    gc_register_coroutine(scanTop, curFrame);
    /* Phase 5: swapcontext 切回 reactor 前，保存协程 vm_state（g_stack_mgr sp /
     * g_try_stack / g_unwind / g_err_jmp / g_thread_root 等）到 co->vm_state，
     * 并恢复 reactor 基线（thread_root=0、空 try 栈等），避免 reactor 主循环
     * resume 另一协程时读到本协程的残留状态。 */
    if (g_co_yield_hook) g_co_yield_hook(co);
    co_set_current(NULL);
#ifdef LM_ASAN_FIBER
    /* 切走：存本协程 fake，声明切回 resume 调用方栈（resume 时记录） */
    __sanitizer_start_switch_fiber(&co->asan_fake, co->asan_caller_bottom,
                                   co->asan_caller_size);
#endif
    swapcontext(&co->ctx, &co->resume_ctx);  /* 切回 resume 调用方 */
#ifdef LM_ASAN_FIBER
    /* 被 resume 切回：恢复本协程 fake */
    __sanitizer_finish_switch_fiber(co->asan_fake, NULL, NULL);
#endif
    /* resume 回来后：移除冻结栈注册，恢复 RUNNING 与 TLS（reactor 调度回来继续执行）。
     * 匹配键须与 register 时一致（stack_base - sizeof(void*)） */
    gc_unregister_coroutine((char*)co->stack_base - sizeof(void*));
    co->state = LM_CO_RUNNING;
    co_set_current(co);
}

int lm_co_is_dead(lm_co_t* co) {
    return co && co->state == LM_CO_DEAD;
}

void lm_co_destroy(lm_co_t* co) {
    if (!co) return;
    /* Phase 5: 销毁前释放 vm_state（由 vm 层 release hook 释放，
     * 避免泄漏协程专属的 VMCoState 结构）。未注册 hook 时 vm_state==NULL，no-op。 */
    if (co->vm_state && g_co_release_hook) g_co_release_hook(co);
    if (co->stack_mmap) {
        long page_l = sysconf(_SC_PAGESIZE);
        if (page_l <= 0) page_l = 4096;
        size_t page = (size_t)page_l;
        size_t total = co->stack_size + page;
        munmap(co->stack_mmap, total);
    }
    free(co);
}
