// lm_co.c —— 有栈协程实现（Phase 8.1：fcontext 汇编切换，ucontext 可回退）
// 切换栈：yield 时整条 C 调用栈冻结。
// 协程内调 lm_co_yield 切回 resume 调用方（reactor 主循环）；
// reactor 调度回来 lm_co_resume 从 yield 点继续。
//
// 协程指针经 lm_ctx_make 的 arg 参数传入 trampoline（fcontext 走伪造帧寄存器槽，
// ucontext 走 makecontext int 参数拆拼），避免 g_startup_co 全局变量的多协程 spawn 竞态。
//
// 栈分配：mmap size + page，末页（高地址方向）mprotect PROT_NONE 作 guard page，
// 爆栈时触发 SIGSEGV 而非静默破坏内存。
#include "lm_co.h"
#include "lm_reactor.h"   /* Phase 8.5：lm_now_ns() 长调度时间戳；R3：now_ms */
#include "lm_stack_pool.h" /* Phase 8.6 C：per-thread 栈池 */
#include "lm_scheduler.h"  /* 销毁时释放 migrate_sched/home_sched 的 scheduler 引用 */
#include "lm_timer.h"      /* R3：coSleep 定时器唤醒 */
#include "lm_sched_stats.h" /* Phase 8.11：存活协程计数 + pending_time 记账 */
#include "gc_runtime.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#include <sys/mman.h>
#include <pthread.h>

/* ============================================================
 * ASAN fiber 注解（仅 AddressSanitizer 构建生效，否则为空操作）
 * 自管栈切换对 ASAN 不可见：协程栈上的合法访问被误判为
 * stack-use-after-scope（google/sanitizers#189 假阳性）。按官方 fiber API
 * 在三个切换点 bookkeeping 告知 ASAN 栈归属：
 *   resume/yield 切走前 __sanitizer_start_switch_fiber（存当前流 fake +
 *   声明目标栈区间）；切入后 __sanitizer_finish_switch_fiber（恢复本流 fake）。
 * entry 结束显式切回 resume_ctx 前，trampoline 补 start_switch 配对。
 * 注解点包在 lm_ctx_jump 调用两侧，与后端（fcontext/ucontext）无关。 */
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
/* LM_ASAN：ASAN 构建生效（与 fiber 注解解耦）。co_swap_out 用 unpoison
 * 规避编译器插桩的局部变量红区（f1/f3）触发的 stack-buffer-underflow 误报。 */
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define LM_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__) && !defined(LM_ASAN)
#  define LM_ASAN 1
#endif
#ifdef LM_ASAN
#include <sanitizer/asan_interface.h>
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
 * Phase 8.6: 全局协程注册表 + can_swap hook
 * 注册表为 intrusive 双向链（co->reg_prev/reg_next），reaper 扫描 idle 候选。
 * spawn 加头、destroy 摘除，reaper 持锁取快照后无锁遍历。
 * can_swap hook 由 VM 层注册，reaper 换出前咨询（未注册=不换出）。
 * ============================================================ */
static pthread_mutex_t g_co_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static lm_co_t* g_co_reg_head = NULL;
static lm_co_can_swap_fn g_co_can_swap_hook = NULL;

void lm_co_set_can_swap_hook(lm_co_can_swap_fn fn) {
    g_co_can_swap_hook = fn;
}

/* 注册表加入（spawn 调用，已分配 co）。持锁 O(1) 头插。 */
static void co_registry_add(lm_co_t* co) {
    co->reg_prev = NULL;
    pthread_mutex_lock(&g_co_reg_lock);
    co->reg_next = g_co_reg_head;
    if (g_co_reg_head) g_co_reg_head->reg_prev = co;
    g_co_reg_head = co;
    pthread_mutex_unlock(&g_co_reg_lock);
    lm_sched_stats_co_created();   /* Phase 8.11：存活协程计数 */
}

/* 注册表摘除（destroy 调用）。持锁 O(1) 双向链摘除。 */
static void co_registry_remove(lm_co_t* co) {
    pthread_mutex_lock(&g_co_reg_lock);
    if (co->reg_prev) co->reg_prev->reg_next = co->reg_next;
    else g_co_reg_head = co->reg_next;   /* 头节点 */
    if (co->reg_next) co->reg_next->reg_prev = co->reg_prev;
    co->reg_prev = NULL;
    co->reg_next = NULL;
    pthread_mutex_unlock(&g_co_reg_lock);
    lm_sched_stats_co_destroyed();   /* Phase 8.11：存活协程计数 */
}

/* Phase 8.13：持锁遍历全部存活协程（sysmon stuck 扫描用）。
 * 与 lm_co_reap_idle 同模式：destroy 阻塞于 g_co_reg_lock，保证回调期间
 * co 不被释放（无快照 UAF 窗口）。回调约束与锁序见头文件注释。 */
void lm_co_registry_foreach(lm_co_foreach_fn fn, void* ctx) {
    if (!fn) return;
    pthread_mutex_lock(&g_co_reg_lock);
    for (lm_co_t* co = g_co_reg_head; co; co = co->reg_next) {
        fn(co, ctx);
    }
    pthread_mutex_unlock(&g_co_reg_lock);
}

/* Phase 8.13：等待退出时释放 waiting_sched 的登记引用。
 * 锁内 exchange：sysmon stuck 扫描持同锁遍历并 retain waiting_sched，
 * 互斥保证 sysmon retain 时 co 必仍持有登记引用（scheduler 不可能在
 * 扫描方 retain 前被释放）。release 放锁外：release 可能触发 scheduler
 * 真销毁（取 scheduler 注册表锁），缩短本锁持有时间。 */
void lm_co_release_waiting_sched(lm_co_t* co) {
    if (!co) return;
    pthread_mutex_lock(&g_co_reg_lock);
    struct lm_scheduler_s* ws = atomic_exchange_explicit(&co->waiting_sched, NULL,
                                                         memory_order_acq_rel);
    pthread_mutex_unlock(&g_co_reg_lock);
    if (ws) lm_scheduler_release(ws);
}

/* ============================================================
 * 协程 trampoline：lm_ctx_make 的 entry，arg 即协程指针（无全局变量竞态）。
 * entry 返回后显式 lm_ctx_jump 切回 resume_ctx（Phase 8.1 起取代 uc_link
 * 隐式链回——fcontext 无此机制，显式切换消除"函数返回触发隐式切换"的隐晦路径，
 * 双后端语义统一）。本函数不得返回（末尾 jump 后 __builtin_unreachable）。
 * ============================================================ */

static void co_trampoline(void* arg) {
    lm_co_t* co = (lm_co_t*)arg;
#ifdef LM_ASAN_FIBER
    /* 首次切入协程：resume 侧 start_switch 已声明本栈，恢复本协程 fake（初始 NULL） */
    __sanitizer_finish_switch_fiber(co->asan_fake, NULL, NULL);
#endif
    /* 进入协程：设 TLS，调 entry（state 已由 resume 方 CAS 写 RUNNING） */
    co_set_current(co);
    if (co->entry) {
        co->entry(co->arg);
    }
    /* entry 返回：协程结束，转 DEAD，清 TLS，显式切回 resume 调用方。 */
    atomic_store_explicit(&co->state, LM_CO_DEAD, memory_order_release);
    co_set_current(NULL);
#ifdef LM_ASAN_FIBER
    /* 切回 resume_ctx 前补配对的 start_switch
     * （存本协程 fake，声明切回目标 caller 栈） */
    __sanitizer_start_switch_fiber(&co->asan_fake, co->asan_caller_bottom,
                                   co->asan_caller_size);
#endif
    lm_ctx_jump(&co->ctx, &co->resume_ctx);
    __builtin_unreachable();   /* DEAD 协程不会被再切入：jump 永不返回 */
}

/* Phase 8.6 C：栈分配已迁至 lm_stack_pool.c（per-thread 池 + mmap+guard）。
 * co_alloc_stack 死代码已删除，spawn/spawn_class 走 lm_stack_pool_get。 */

/* ============================================================
 * API
 * ============================================================ */

lm_co_t* lm_co_spawn(lm_co_entry_t entry, void* arg, size_t stack_size) {
    if (stack_size == 0) stack_size = LM_CO_DEFAULT_STACK_SIZE;
    /* Phase 8.6：按 stack_size 推断栈档（≤SMALL→SMALL 桶，否则 NORMAL）。 */
    int stack_class = (stack_size <= LM_STACK_SMALL) ? LM_STACK_CLASS_SMALL
                                                     : LM_STACK_CLASS_NORMAL;
    lm_co_t* co = (lm_co_t*)calloc(1, sizeof(lm_co_t));
    if (!co) return NULL;
    /* Phase 8.6 C：从 per-thread 栈池取栈（池命中复用，池空 mmap 新栈）。 */
    lm_stack_storage_t st;
    if (lm_stack_pool_get(stack_class, &st) != 0) {
        free(co);
        return NULL;
    }
    co->stack_mmap = st.mmap_base;   /* 低地址，return 到池用 */
    co->stack_size = st.stack_size;
    co->stack_base = st.stack_base; /* 高地址，栈基址 */
    co->stack_top  = st.stack_top;   /* 低地址，可用区起 */
    co->entry = entry;
    co->arg = arg;
    atomic_store_explicit(&co->state, LM_CO_READY, memory_order_relaxed);
    co->next = NULL;
    /* Phase 8.2：调度分流标志初始化。
     * stealable=1：未运行的协程无 VM 栈数据，任何线程 resume 都安全；
     *             首次 yield 后由 vm hook 按是否有 mig_copy 重判。
     * pinned 继承父协程（协程内 spawn 场景）：连接 handler 等 pinned 协程
     * 派生的子协程默认同线程亲和；顶层 spawn（无父协程）pinned=0。 */
    lm_co_t* parent = lm_co_current();
    co->pinned = parent ? parent->pinned : 0;
    atomic_store_explicit(&co->stealable, 1, memory_order_relaxed);
    /* Phase 8.5：reduction 预算初始化。首次 resume 时装载 LM_SCHED_REDS。 */
    co->reds = LM_SCHED_REDS;
    co->last_resume_ns = 0;
    atomic_init(&co->preempt_flag, 0);
    co->slice_yield = 0;
    /* Phase 8.6：栈分级 + 换出字段初始化 + 注册表加入。
     * swap_state=NORMAL（未换出）；swap_buffer=NULL；reg 链由 co_registry_add 设。 */
    co->stack_class = stack_class;
    co->swap_buffer = NULL;
    co->swap_size = 0;
    atomic_init(&co->swap_state, LM_CO_SWAP_NORMAL);
    /* Phase 8.13：等待源登记字段初始化（calloc 已清零，显式 init 表意）。 */
    atomic_init(&co->wait_kind, LM_WAIT_NONE);
    atomic_init(&co->waiting_sched, NULL);
    atomic_init(&co->stuck_votes, 0);
    atomic_init(&co->sleep_ctx, NULL);
    co_registry_add(co);

    /* 构造初始上下文：栈 [mmap_base, mmap_base+stack_size)，首次 resume 时
     * 从 co_trampoline(co) 开始执行（fcontext 在栈顶伪造帧；ucontext 包装
     * getcontext/makecontext）。entry 返回由 trampoline 显式切回 resume_ctx。 */
    lm_ctx_make(&co->ctx, st.mmap_base, st.stack_size, co_trampoline, co);
    return co;
}

/* Phase 8.6：显式指定栈档的 spawn（轻量 C 协程走 SMALL 档）。
 * size 由 stack_class 决定：SMALL=16KiB / NORMAL=128KiB。 */
lm_co_t* lm_co_spawn_class(lm_co_entry_t entry, void* arg, int stack_class) {
    lm_co_t* co = (lm_co_t*)calloc(1, sizeof(lm_co_t));
    if (!co) return NULL;
    lm_stack_storage_t st;
    if (lm_stack_pool_get(stack_class, &st) != 0) {
        free(co);
        return NULL;
    }
    co->stack_mmap = st.mmap_base;
    co->stack_size = st.stack_size;
    co->stack_base = st.stack_base;
    co->stack_top  = st.stack_top;
    co->entry = entry;
    co->arg = arg;
    atomic_store_explicit(&co->state, LM_CO_READY, memory_order_relaxed);
    co->next = NULL;
    lm_co_t* parent = lm_co_current();
    co->pinned = parent ? parent->pinned : 0;
    atomic_store_explicit(&co->stealable, 1, memory_order_relaxed);
    co->reds = LM_SCHED_REDS;
    co->last_resume_ns = 0;
    atomic_init(&co->preempt_flag, 0);
    co->slice_yield = 0;
    co->stack_class = stack_class;
    co->swap_buffer = NULL;
    co->swap_size = 0;
    atomic_init(&co->swap_state, LM_CO_SWAP_NORMAL);
    /* Phase 8.13：等待源登记字段初始化（calloc 已清零，显式 init 表意）。 */
    atomic_init(&co->wait_kind, LM_WAIT_NONE);
    atomic_init(&co->waiting_sched, NULL);
    atomic_init(&co->stuck_votes, 0);
    atomic_init(&co->sleep_ctx, NULL);
    co_registry_add(co);
    lm_ctx_make(&co->ctx, st.mmap_base, st.stack_size, co_trampoline, co);
    return co;
}

/* ============================================================
 * Phase 8.6 E: idle 换入（唤醒方 resume 前调）
 * DONTNEED 版：mmap 映射保留，swap_out 仅 DONTNEED 物理页 + 内容存 buffer。
 * 换入只需 memcpy buffer 回原偏移（ctx.sp 不变，基址不变）→ 物理页重新 fault-in。
 * GC：注销 buffer 区间（协程离开 suspended 态，运行期由线程 GCThreadEntry 扫描，
 *   不进协程栈表；下次 yield 再注册）。swap_in 与 resume 间无 safepoint，无漏根窗口。
 * 竞态：CAS SWAPPED→SWAPPING_IN；若 reaper 正在 SWAPPING_OUT，自旋等 SWAPPED。
 * ============================================================ */
static void co_swap_in(lm_co_t* co) {
    /* 等 reaper 完成 swap_out（SWAPPING_OUT→SWAPPED）。短暂自旋，reaper 拷贝<1ms。 */
    for (;;) {
        int s = atomic_load_explicit(&co->swap_state, memory_order_acquire);
        if (s == LM_CO_SWAP_SWAPPED) {
            int expected = LM_CO_SWAP_SWAPPED;
            if (atomic_compare_exchange_strong(&co->swap_state, &expected,
                                                LM_CO_SWAP_SWAPPING_IN)) {
                break;
            }
        } else if (s == LM_CO_SWAP_NORMAL) {
            /* 他人已换入或 swap_out 已回退，无需换入。 */
            return;
        } else {
            /* SWAPPING_OUT / SWAPPING_IN：短暂退避重试。 */
            struct timespec ts = {0, 1000};   /* 1µs */
            nanosleep(&ts, NULL);
        }
    }
    /* 换入：memcpy buffer 内容回原偏移（ctx.sp 不变，基址不变）。
     * DONTNEED 释放的物理页被 memcpy 写访问重新 fault-in（恢复内容）。 */
    if (co->swap_buffer && co->swap_size) {
#ifdef LM_ASAN
        /* ASAN：swap_out 时已 unpoison 整段，但保险起见再 unpoison 一次
         *（防 DONTNEED 后 ASAN shadow 状态被改回——实际不会，但 memcpy 写
         * 回时 ASAN 拦截器可能仍按红区拒绝写入）。 */
        __asan_unpoison_memory_region(co->ctx.sp, co->swap_size);
#endif
        memcpy(co->ctx.sp, co->swap_buffer, co->swap_size);
    }
    /* GC 注销 buffer 区间（协程将运行，不进协程栈表）。
     * buffer 注册键 = buffer+swap_size（swap_out 第 6 步注册时的 stack_top）。 */
    gc_unregister_coroutine((char*)co->swap_buffer + co->swap_size);
    free(co->swap_buffer);
    co->swap_buffer = NULL;
    co->swap_size   = 0;
    atomic_store_explicit(&co->stealable, 1, memory_order_release);   /* 恢复换出前状态（swap 仅针对 stealable=1 协程） */
    atomic_store_explicit(&co->swap_state, LM_CO_SWAP_NORMAL, memory_order_release);
}

void lm_co_resume(lm_co_t* co) {
    if (!co || atomic_load_explicit(&co->state, memory_order_acquire) == LM_CO_DEAD) return;
    /* Phase 7.2：scheduler 投递在 SPAWN 时做（BUILTIN_CO_SPAWN 检查
     * sched->current==NULL 时 post 到就绪队列），resume 走纯直连切换
     * 路径——reactor drain_ready 钩子直接调本函数消费就绪队列。
     * 协程内 spawn（current!=NULL）不投递，由 spawning 协程显式 resume。 */
    /* 保存当前（resume 调用方）上下文到 co->resume_ctx，切到 co->ctx。
     * caller 可能是 NULL（reactor 主循环）或另一个协程（协程内嵌套 resume，
     * 如 server_co accept 后 spawn echo_co 并 resume 启动）。 */
    lm_co_t* caller = lm_co_current();
    /* Phase 8.6 修复：与 reaper（co_swap_out）的栈仲裁——CAS 抢 RUNNING。
     * LM_CO_SWAPPING = reaper 正在搬栈：自旋等其完成（<1ms）；完成后
     * swap_state=SWAPPED，下轮循环先 swap_in 恢复栈内容再 CAS。
     * 无仲裁时 reaper 可能在 resume 切入前 madvise 清空活栈（compute_test rc=139）。
     * Phase 8.6 E：换出态协程 resume 前先换入（SWAPPING_OUT 态也要等）。 */
    lm_co_state_t prev;
    for (;;) {
        int ss = atomic_load_explicit(&co->swap_state, memory_order_acquire);
        if (ss == LM_CO_SWAP_SWAPPED || ss == LM_CO_SWAP_SWAPPING_OUT) {
            co_swap_in(co);
        }
        lm_co_state_t st = atomic_load_explicit(&co->state, memory_order_acquire);
        if (st == LM_CO_SWAPPING) {
            struct timespec ts = {0, 1000};   /* 1µs 退避，reaper 拷贝 <1ms */
            nanosleep(&ts, NULL);
            continue;
        }
        if (st == LM_CO_SUSPENDED || st == LM_CO_READY) {
            if (atomic_compare_exchange_weak_explicit(&co->state, &st,
                    LM_CO_RUNNING, memory_order_acq_rel, memory_order_acquire)) {
                prev = st;
                break;
            }
            continue;
        }
        prev = st;   /* 其他态（理论不可达）：保持现行直接覆盖语义 */
        break;
    }
    /* Phase 8.5：每次 resume 重装 reduction 预算 + 记录起始时间。
     * 预算耗尽（VM 派发点 / cc 插桩点）在指令边界 lm_co_yield() 让出，
     * 切回这里时重新装载——context_switch/schedule 同语义。 */
    co->reds = LM_SCHED_REDS;
    co->last_resume_ns = lm_now_ns();
    /* Phase 8.11：pending_time 记账——经就绪队列调度的协程（ready_ts!=0）
     * 统计 ready→被执行延迟入全局直方图；直连 resume（ready_ts==0）跳过。 */
    {
        uint64_t rts = atomic_exchange_explicit(&co->ready_ts, 0,
                                                memory_order_acq_rel);
        if (rts && co->last_resume_ns > rts) {
            lm_sched_stats_record_pending_ns(co->last_resume_ns - rts);
        }
    }
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
    lm_ctx_jump(&co->resume_ctx, &co->ctx);
#ifdef LM_ASAN_FIBER
    /* 协程 yield/结束切回这里：恢复 caller 自己的 fake */
    __sanitizer_finish_switch_fiber(caller ? caller->asan_fake : tl_main_fake,
                                    NULL, NULL);
#endif
    /* 协程 yield 或 entry 返回后切回这里。
     * yield 切回（state 仍 RUNNING）：协程栈此刻已冻结（ctx.sp 已保存），
     * 由本线程补写 SUSPENDED——reaper 仅见 SUSPENDED 才能 CAS SWAPPING 动栈，
     * 保证「state 可见 = 栈已冻结」的时序（若由 yield 在 jump 前写，
     * reaper 可能在栈未冻结时搬栈 + madvise 清空活栈）。
     * DEAD 切回（trampoline 已写 DEAD）：调 yield_hook 恢复 baseline
     *（DEAD 时跳过保存，只恢复 baseline——协程要销毁，保存无意义）。 */
    lm_co_state_t stAfter = atomic_load_explicit(&co->state, memory_order_acquire);
    if (stAfter == LM_CO_RUNNING) {
        atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
    } else if (stAfter == LM_CO_DEAD && g_co_yield_hook) {
        g_co_yield_hook(co);
    }
    /* 恢复 caller 的 current：NULL=reactor 主循环无协程上下文；非 NULL=外层协程
     * （嵌套 resume 场景，外层协程继续执行时 lm_co_current() 需返回外层 co，
     * 否则后续 in_co 判定失效走阻塞分支）。 */
    co_set_current(caller);
    (void)prev;
    /* Phase 8.5 统一收敛：slice_yield（VM 指令边界 LM_BUMP_REDS 预算耗尽让出）
     * 的重入队在所有 resume 路径返回点统一处理。
     * 此前仅 drain_ready 路径处理 slice_yield 重入队；BUILTIN_CO_RESUME
     * （lumin 层 co.resume()/h.start()）等业务层直连 resume 路径不处理——
     * 协程让出后永久丢失（HTTP header hang bug 根因：CPU 密集 handler 在
     * JMP 指令边界 slice_yield 让出后无人重入队，响应永不发出）。
     * 与 drain_ready 的重复防护：drain_ready 已删除其 slice_yield 块，
     * 统一由本点处理，避免双重入队。
     * 迁移互斥：migrate_sched 非空（computeBegin/sysmon 强制迁移）时由
     * handle_migrate 投递目标 scheduler，本点跳过（否则双线程同栈 UB）。 */
    if (co->slice_yield) {
        co->slice_yield = 0;
        if (atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_DEAD &&
            atomic_load_explicit(&co->migrate_sched, memory_order_acquire) == NULL) {
            lm_scheduler_t* sched = lm_scheduler_get_current();
            if (sched) {
                lm_sched_stats_force_yield();   /* reduction 账本计数 */
                /* 重入队到 mutex FIFO 队尾（非 WSQ LIFO），保证时间片轮转公平：
                 * 刚让出的 CPU 密集协程排到队尾，WSQ 中其他协程先跑。 */
                lm_scheduler_post(sched, co);
            }
        }
    }
}

/* 时间片抢占安全门：VM 层注册，返回当前 4 操作数栈是否处于本协程入口
 * 基线（sp 平衡，让出不会暴露栈上活值）。未注册=无共享操作数栈（纯 C），
 * 可直接让出。 */
static lm_co_can_preempt_hook_t g_co_can_preempt_hook = NULL;
void lm_co_set_can_preempt_hook(lm_co_can_preempt_hook_t cb) {
    g_co_can_preempt_hook = cb;
}

/* 时间片耗尽的统一抢占动作：仅在操作数栈平衡时让出。非平衡（CALL/BUILTIN
 * 派发点实参含 receiver 正压栈）时让出会把活值留在 per-thread 共享栈数据
 * 区，被同线程后运行的协程覆盖，resume 后实参/receiver 串改成他人对象
 * （file.recv 跨协程类型串改根因）。此时借一个时间片继续，平衡派发点
 *  （JMP 回边、弹参后的 callee 派发点）有界可达，不影响轮转公平。 */
void lm_co_slice_bump(lm_co_t* co) {
    if (!co) return;
    if (g_co_can_preempt_hook && !g_co_can_preempt_hook(co)) {
        co->reds = LM_SCHED_REDS;
        return;
    }
    co->slice_yield = 1;   /* 标记时间片耗尽，drain_ready 重入队 */
    lm_co_yield();
}

/* Phase 8.5 J：C 内置检查点原语。
 * 长 C 内置（json parse/dump、sort 等）在主循环周期调用。
 * 语义 = LM_BUMP_REDS：扣 1 预算，归零时经 can_preempt 门控决定让出/借片。
 * 让出后 C 栈随协程冻结（fcontext），resume 后沿原栈继续——
 * 局部变量、循环计数器、qsort 内部状态全部保留，无需显式续体。
 * 非协程上下文（co==NULL，main/纯 C 线程）直接返回。 */
void lm_co_builtin_checkpoint(void) {
    lm_co_t* co = lm_co_current();
    if (!co) return;
    LM_BUMP_REDS(co);
}

/* 按量扣减变体：字节计费场景（json 等），契约见 lm_co.h。 */
void lm_co_builtin_checkpoint_n(long n) {
    lm_co_t* co = lm_co_current();
    if (!co) return;
    LM_BUMP_REDS_N(co, n);
}

void lm_co_yield(void) {
    lm_co_t* co = lm_co_current();
    if (!co || atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_RUNNING) return;
    /* 注意：SUSPENDED 不在此写——jump 后栈才冻结，由 resume 方切回后补写
     *（见 lm_co_resume 返回段注释），保证 reaper 见 SUSPENDED 时栈已冻结。 */
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
#ifdef LM_CTX_FCONTEXT
    /* fcontext：jump 把 callee-saved 现场 push 到协程栈、ctx.sp 指向保存区
     * 最低点。扫描下界必须间接读 ctx.sp（此刻旧值，jump 后才更新为冻结 sp）——
     * 若下界只到本帧 rbp，寄存器保存区被切出扫描区间：-O2 编译的调用方把
     * GC 指针驻留 callee-saved 寄存器（%rbx 等）时漏标 → sweep 误回收 → UAF
     *（co_gc_test -O2 实测定案：Clang 对不取地址的 volatile 局部折叠进 %rbx）。 */
    gc_register_coroutine(scanTop, curFrame, (void**)&co->ctx.sp);
#else
    /* ucontext 回退：寄存器现场在 ucontext_t 内（非栈扫描区间），维持旧行为。 */
    gc_register_coroutine(scanTop, curFrame, NULL);
#endif
    /* Phase 5: swapcontext 切回 reactor 前，保存协程 vm_state（g_stack_mgr sp /
     * g_try_stack / g_unwind / g_err_jmp / g_thread_root 等）到 co->vm_state，
     * 并恢复 reactor 基线（thread_root=0、空 try 栈等），避免 reactor 主循环
     * resume 另一协程时读到本协程的残留状态。 */
    if (g_co_yield_hook) g_co_yield_hook(co);
    co_set_current(NULL);
#ifdef LM_ASAN_FIBER
    __sanitizer_start_switch_fiber(&co->asan_fake, co->asan_caller_bottom,
                                   co->asan_caller_size);
#endif
    lm_ctx_jump(&co->ctx, &co->resume_ctx);  /* 切回 resume 调用方 */
#ifdef LM_ASAN_FIBER
    __sanitizer_finish_switch_fiber(co->asan_fake, NULL, NULL);
#endif
    /* resume 回来后：移除冻结栈注册，恢复 TLS（reactor 调度回来继续执行）。
     * state 已由 resume 方 CAS 写 RUNNING，此处不再写。
     * 匹配键须与 register 时一致（stack_base - sizeof(void*)） */
    gc_unregister_coroutine((char*)co->stack_base - sizeof(void*));
    co_set_current(co);
}

int lm_co_is_dead(lm_co_t* co) {
    return co && atomic_load_explicit(&co->state, memory_order_acquire) == LM_CO_DEAD;
}

/* ============================================================
 * R3：coSleep 协程友好休眠（定时器 + yield，不阻塞 reactor 线程）
 * ============================================================ */
/* 挂起上下文：协程侧与 destroy 侧经 co->sleep_ctx exchange 取走所有权，
 * 取消成功（返回 0=回调未运行）的一方 free；回调已触发（1=运行中 / -1=已执行）
 * 时由回调方 free——与 fd 等待 waitCtx 所有权语义对齐。 */
typedef struct {
    lm_co_t* co;
    lm_scheduler_t* sched;   /* 唤醒目标（登记时当前线程 scheduler 快照） */
    lm_timer_id_t id;
} CoSleepCtx;

/* 定时器回调（timer 线程）：投递协程回 scheduler（post + self-pipe）。
 * MUST NOT block（timer 线程契约）。提前触发场景（回调先于 yield 到期）：
 * wakeup 仅入队 + 置 queued，协程随后 yield 由 drain_ready 恢复，语义不变。 */
/* coSleep 完成握手值（co->sleep_arm）。 */
#define LM_COSLEEP_ARMED 1   /* 定时器已注册，回调尚未投递唤醒 */
#define LM_COSLEEP_FIRED 2   /* 回调已投递唤醒并结束（此后不再访问 ctx/co） */

static void co_sleep_timer_cb(lm_timer_id_t id, void* arg) {
    (void)id;
    CoSleepCtx* ctx = (CoSleepCtx*)arg;
    /* 回调只投递唤醒 + 置完成位，绝不 free：ctx 由协程侧（正常续行/
     * lm_co_destroy）单一所有者释放。读 ctx->sched/co 期间二者必存活——
     * 正常续行先等到 FIRED 才释放 ctx；destroy 先 cancel，见回调在跑则等
     * FIRED 后才释放协程。FIRED 是回调最后一个动作，置位后不再触碰任何共享
     * 状态（修复旧版回调 free(ctx) 与清理方解引用 ctx 的 heap-use-after-free，
     * 该 UAF 在 reap 批量释放/复用协程内存时被放大成服务停滞）。 */
    lm_scheduler_wakeup(ctx->sched, ctx->co);
    atomic_store_explicit(&ctx->co->sleep_arm, LM_COSLEEP_FIRED, memory_order_release);
}

/* 等定时器回调结束（sleep_arm==FIRED）。回调在独立 timer 线程，仅一次投递 +
 * 一个原子 store，极短；调用方为 reactor/销毁线程（永不是 timer 线程），自旋
 * sched_yield 不影响回调运行。 */
static void co_sleep_wait_fired(lm_co_t* co) {
    while (atomic_load_explicit(&co->sleep_arm, memory_order_acquire) != LM_COSLEEP_FIRED) {
        sched_yield();
    }
}

/* 协程正常唤醒续行后的清理：本协程既被唤醒，回调必已投递，等其结束（FIRED）
 * 再由协程侧释放 ctx。回调不释放，故不存在双方 free / 悬垂解引用。 */
static void co_sleep_after_wait(lm_co_t* co) {
    co_sleep_wait_fired(co);
    CoSleepCtx* ctx = (CoSleepCtx*)atomic_exchange_explicit(&co->sleep_ctx, NULL,
                                                            memory_order_acq_rel);
    if (ctx) free(ctx);
}

/* lm_codestroy 路径：协程可能仍挂在 coSleep 定时器上（未到期）。
 * cancel 仲裁定时器状态：
 *   rc==0（SCHEDULED→DONE 成功）：回调保证不再运行，直接释放 ctx；
 *   rc==1/-1（RUNNING/已执行）：回调在跑或已投递唤醒，等 FIRED（回调不 free，
 *   ctx 仍有效）后释放——保证随后 free(co) 时回调不再访问协程。 */
static void co_sleep_cancel_on_destroy(lm_co_t* co) {
    CoSleepCtx* ctx = (CoSleepCtx*)atomic_exchange_explicit(&co->sleep_ctx, NULL,
                                                            memory_order_acq_rel);
    if (!ctx) return;
    if (lm_timer_cancel(ctx->id) == 0) {
        free(ctx);
        return;
    }
    co_sleep_wait_fired(co);
    free(ctx);
}

/* 协程友好休眠 ms 毫秒：挂起当前协程，集中定时器线程到期后投递唤醒。
 * 返回 0 成功；-1 非协程上下文或无 scheduler（框架协程均满足）；
 * -2 定时器注册失败。
 * 与 fd 等待的区别：唯一唤醒方是定时器回调，无关闭/事件竞态，无需三态
 * 仲裁字；不登记 wait_kind（LM_WAIT_NONE）——sysmon 跳过扫描，休眠时长
 * 有界（上层封顶 5s），无 stuck 风险。 */
int lm_co_sleep_ms(long long ms) {
    lm_co_t* co = lm_co_current();
    if (!co) return -1;
    if (ms <= 0) return 0;
    lm_scheduler_t* sched = lm_scheduler_get_current();
    if (!sched) return -1;
    CoSleepCtx* ctx = (CoSleepCtx*)malloc(sizeof(CoSleepCtx));
    if (!ctx) return -2;
    ctx->co = co;
    ctx->sched = sched;
    ctx->id = LM_TIMER_INVALID_ID;
    /* 先置 ARMED 并发布 sleep_ctx，再注册定时器：回调一经 lm_timer_add 即可能
     * 运行（时钟抖动 / deadline 已过期），其读取的 co/sched 必须先就位；
     * ctx->id 回调不读，仅 destroy 取消时读（那时 add 必已返回）。 */
    atomic_store_explicit(&co->sleep_arm, LM_COSLEEP_ARMED, memory_order_relaxed);
    atomic_store_explicit(&co->sleep_ctx, ctx, memory_order_release);
    lm_timer_id_t tid = lm_timer_add(lm_reactor_now_ms() + (uint64_t)ms,
                                     co_sleep_timer_cb, ctx);
    if (tid == LM_TIMER_INVALID_ID) {
        atomic_exchange_explicit(&co->sleep_ctx, NULL, memory_order_acq_rel);
        free(ctx);
        return -2;
    }
    ctx->id = tid;
    lm_co_yield();
    /* 被唤醒：等回调投递并结束（FIRED）后由协程侧释放 ctx（回调不 free）。
     * 即使回调在 yield 前提前触发（deadline 已过期），也只是立即就绪，语义
     * 退化为 0ms 睡眠，无 UAF。 */
    co_sleep_after_wait(co);
    return 0;
}

void lm_co_destroy(lm_co_t* co) {
    if (!co) return;
    /* R3：协程仍挂起在 coSleep 定时器上被强制 destroy（accept 协程退避中
     * 随 run 退出清理）→ 取消定时器并等在飞回调结束，防回调 wakeup/访问
     * 已释放协程（UAF）。已正常续行（FIRED）时此处 sleep_ctx 为 NULL，no-op。 */
    co_sleep_cancel_on_destroy(co);
    /* Phase 8.6：先从全局注册表摘除（reaper 不再扫到本协程）。 */
    co_registry_remove(co);
    /* Phase 5: 销毁前释放 vm_state（由 vm 层 release hook 释放，
     * 避免泄漏协程专属的 VMCoState 结构）。未注册 hook 时 vm_state==NULL，no-op。 */
    if (co->vm_state && g_co_release_hook) g_co_release_hook(co);
    /* Phase 8.6：若协程已换出（SWAPPED），栈已归还池/未持有，释放堆 buffer。
     * 子阶段 C/D 接栈池后：未换出时归还栈到池；换出时 free(buffer)。
     * 本期仍走 munmap（池化在 B/C 接入）。 */
    int swapped = atomic_load_explicit(&co->swap_state, memory_order_acquire);
    if (swapped == LM_CO_SWAP_SWAPPED || swapped == LM_CO_SWAP_SWAPPING_OUT) {
        /* Phase 8.6 D：换出态——mmap 映射保留（基址不变语义），内容在 buffer。
         * destroy 需 free buffer + munmap 保留的映射（不再归还池，因内容已废）。 */
        if (co->swap_buffer) {
            free(co->swap_buffer);
            co->swap_buffer = NULL;
        }
        if (co->stack_mmap) {
            long page_l = sysconf(_SC_PAGESIZE);
            if (page_l <= 0) page_l = 4096;
            munmap(co->stack_mmap, co->stack_size + (size_t)page_l);
        }
    } else if (co->stack_mmap) {
        /* Phase 8.6 C：未换出的协程归还栈到 per-thread 池（池满才 munmap）。
         * poison 由 lm_stack_pool_return 内部完成。 */
        long page_l = sysconf(_SC_PAGESIZE);
        if (page_l <= 0) page_l = 4096;
        lm_stack_storage_t st = {
            co->stack_mmap,
            co->stack_base,
            co->stack_top,
            co->stack_size,
            co->stack_size + (size_t)page_l
        };
        lm_stack_pool_return(&st);
    }
    /* 引用计数兜底：正常路径 handle_migrate 已消费 migrate_sched、computeEnd
     * 已转移 home_sched；此处覆盖异常路径（co 未 resume 即销毁、迁移协议中断），
     * 释放遗留引用防 scheduler 泄漏。原子取出置 NULL，与 sysmon try_set 无竞态。 */
    struct lm_scheduler_s* ms = atomic_exchange_explicit(&co->migrate_sched, NULL,
                                                         memory_order_acq_rel);
    if (ms) lm_scheduler_release(ms);
    struct lm_scheduler_s* hs = atomic_exchange_explicit(&co->home_sched, NULL,
                                                         memory_order_acq_rel);
    if (hs) lm_scheduler_release(hs);
    /* Phase 8.13：butex 等待登记时 retain 的 waiting_sched 引用兜底释放
     *（正常等待退出路径已 lm_co_release_waiting_sched；此处覆盖协程仍挂在
     * butex 上被强制 destroy 的异常路径）。协程已先从注册表摘除（本函数开头
     * co_registry_remove），sysmon 扫描不再能触及本 co，无 retain 竞态，
     * 直接 exchange+release 即可（无需再过注册表锁）。 */
    struct lm_scheduler_s* ws = atomic_exchange_explicit(&co->waiting_sched, NULL,
                                                         memory_order_acq_rel);
    if (ws) lm_scheduler_release(ws);

    /* Phase 8.8：协程仍挂在 fd 等待字上被强制 destroy（accept/handler 挂起协程
     * 随框架 destroyActive / acceptCo.destroy 释放，未走 co_wait_fd_timeout 正常
     * cleanup）→ 清理等待，防后续 close_notify 读到悬垂 co（UAF）。
     * CAS WAITING(self)→CLOSED 解仲裁（事件/超时/close 三方见此字非 WAITING 不再
     * 争抢本协程），并清 conn->co 使 close_notify 空指针短路。
     * 同线程串行：destroy 与 close_notify 均在协程所属 reactor 线程，无竞态。
     * Phase 8.13：本清理仅对 fd 等待（wait_kind==LM_WAIT_FD）生效——
     * butex 等待登记的 waiting_word 指向 32 位 butex 字，8 字节 CAS 会越界
     * 读写相邻内存；butex 路径由 waiting_sched 引用托管（上方已清理）。 */
    _Atomic uintptr_t* ww = atomic_load_explicit(&co->waiting_word, memory_order_acquire);
    if (ww && atomic_load_explicit(&co->wait_kind, memory_order_acquire) == LM_WAIT_FD) {
        lm_connection_t* wconn = (lm_connection_t*)co->waiting_conn;
        uintptr_t exp = (uintptr_t)co;
        atomic_compare_exchange_strong_explicit(ww, &exp, LM_FD_CLOSED,
                                                memory_order_acq_rel, memory_order_acquire);
        if (wconn && wconn->co == co) wconn->co = NULL;
        atomic_store_explicit(&co->waiting_word, NULL, memory_order_release);
        co->waiting_conn = NULL;
    }

    free(co);
}

/* ============================================================
 * Phase 8.6 D: idle 换出（堆栈缓冲降级——仅 idle>30s 执行）
 * 在 reaper（sysmon）线程调用，co 不在任何线程栈上（state==SUSPENDED）。
 *
 * 步骤（对齐调研 §8.6 第 3 条 + 栈换出协议）：
 *   1. 条件预检（SUSPENDED + idle>阈值 + can_swap 通过 + 非迁移中）
 *   2. CAS swap_state NORMAL→SWAPPING_OUT（仲裁与唤醒方 swap_in 竞态）
 *   3. 计算已用栈深 used = stack_base - ctx.sp（fcontext 冻结 sp）
 *   4. malloc 堆 buffer + memcpy 已用栈段
 *   5. GC：先注册 buffer 区间，再注销旧栈区间（保证任意时刻至少一区注册，
 *      防 GC 标记窗口漏根 → UAF）
 *   6. 归还 mmap 栈到池
 *   7. 清栈字段，置 swap_buffer/size + SWAPPED + stealable=0
 * 返回 1=已换出，0=未换出（条件不满足/CAS 失败）。
 * ============================================================ */
static int co_swap_out(lm_co_t* co, uint64_t now_ns) {
    /* 1. 条件预检（原子读，快速跳过非候选）。 */
    if (atomic_load_explicit(&co->state, memory_order_acquire) != LM_CO_SUSPENDED) return 0;
    if (atomic_load_explicit(&co->migrate_sched, memory_order_acquire) != NULL) return 0;   /* 迁移中，不动其栈 */
    if (atomic_load_explicit(&co->stealable, memory_order_acquire) == 0) return 0;   /* 已绑定线程（compute 迁移态），跳过 */
    int ss = atomic_load_explicit(&co->swap_state, memory_order_acquire);
    if (ss != LM_CO_SWAP_NORMAL) return 0;     /* 已换出/换入中 */
    /* idle 时长：last_resume_ns 是上次 resume 时间。SUSPENDED 协程若长期未
     * 被 resume，now - last_resume_ns > 阈值即 idle 候选。
     * last_resume_ns==0 = 从未 resume（READY 不会进 SUSPENDED 分支，防 0 下溢）。 */
    if (co->last_resume_ns == 0) return 0;
    if (now_ns - co->last_resume_ns <= LM_REAP_IDLE_THRESHOLD_NS) return 0;
    /* can_swap hook：VM 层否决则跳过。未注册 hook = 不换出（runtime 不猜栈内布局）。 */
    if (g_co_can_swap_hook && !g_co_can_swap_hook(co)) return 0;
#ifndef LM_CTX_FCONTEXT
    /* ucontext 后端：寄存器现场在 ucontext_t 内不在栈上，换入需重做 makecontext，
     * 复杂度高；本期禁用换出（can_swap 未注册时本就跳过，此处双保险）。 */
    return 0;
#else
    /* 2. 栈仲裁①：CAS state SUSPENDED→SWAPPING（修复 resume vs reaper 竞态）。
     *    resume 侧见 SWAPPING 自旋等待（见 lm_co_resume），CAS 赢则本线程独占协程栈。
     *    SUSPENDED 由 resume 方在协程 jump 切回后补写（栈已冻结），
     *    故 CAS 成功即保证栈已冻结（ctx.sp 已保存），可安全 memcpy/madvise。 */
    {
        lm_co_state_t expectedSt = LM_CO_SUSPENDED;
        if (!atomic_compare_exchange_strong_explicit(&co->state, &expectedSt,
                LM_CO_SWAPPING, memory_order_acq_rel, memory_order_acquire)) {
            return 0;   /* 被 resume 抢到 RUNNING / 状态已变，放弃 */
        }
    }
    /* 3. 栈仲裁②：CAS swap_state NORMAL→SWAPPING_OUT（仲裁与唤醒方 swap_in 竞态）。 */
    int expected = LM_CO_SWAP_NORMAL;
    if (!atomic_compare_exchange_strong(&co->swap_state, &expected,
                                        LM_CO_SWAP_SWAPPING_OUT)) {
        atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
        return 0;   /* 被唤醒方抢到，回退栈仲裁后放弃 */
    }
    /* 4. 计算已用栈深：[ctx.sp（冻结低地址）, stack_base（高地址）)。
     *    fcontext：jump 把 callee-saved 现场 push 到协程栈、ctx.sp 指向保存区
     *    最低点（lm_coro_ctx.h:43-48），故 [ctx.sp, stack_base) 含全部冻结帧。 */
    void* sp = co->ctx.sp;
    void* base = co->stack_base;
    if (!sp || !base || (char*)base <= (char*)sp) {
        /* 栈深 0 或异常（sp>=base），无可换出。回退双重仲裁。 */
        atomic_store_explicit(&co->swap_state, LM_CO_SWAP_NORMAL,
                              memory_order_release);
        atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
        return 0;
    }
    size_t used = (size_t)((char*)base - (char*)sp);
    /* 5. malloc 堆 buffer + memcpy 已用栈段（堆栈缓冲）。 */
    void* buffer = malloc(used);
    if (!buffer) {
        atomic_store_explicit(&co->swap_state, LM_CO_SWAP_NORMAL,
                              memory_order_release);
        atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
        return 0;
    }
#ifdef LM_ASAN
    /* ASAN：编译器给协程栈内局部变量插桩的红区（f1=Stack left redzone /
     * f3=Stack right redzone）会让本函数 memcpy 整段栈触发
     * stack-buffer-underflow 误报（红区在 sp→base 之间）。
     * 换出前 unpoison 整个 used 区间清除红区标记 → memcpy 通过；
     * 后续 madvise DONTNEED 释放物理页，ASAN shadow 仍记 00（addressable），
     * 但物理页不可访问——swap_in memcpy 写回触发 fault-in 重新分页，
     * 协程 resume 时编译器插桩在 scope 重入点重建局部变量红区。 */
    __asan_unpoison_memory_region(sp, used);
#endif
    memcpy(buffer, sp, used);
    /* 6. GC 区间切换（顺序：先注册 buffer，再注销旧栈——保证任意时刻至少
     *    一区注册，防 GC 标记窗口漏根 → UAF。buffer 内容=旧栈拷贝，双注册
     *    期间保守扫描重复标根无害）。 */
    gc_register_coroutine((char*)buffer + used, buffer, NULL);
    gc_unregister_coroutine((char*)base - sizeof(void*));
    /* 7. 释放栈物理页：内容已在 buffer。
     *
     *   Linux：madvise(MADV_DONTNEED) 立即丢弃匿名私有页，mmap 映射保留
     *          （基址不变），swap_in memcpy 写回时重新 zero-fault。
     *
     *   macOS(Darwin)：MADV_DONTNEED / MADV_FREE 对匿名页均为惰性回收
     *          （无内存压力不归还，RSS 实测不降；最小实测 3000 映射
     *          madvise 返回 0 但 RSS 变化为 0）。改用 munmap + MAP_FIXED
     *          原地重建映射立即归还物理页，重建后基址不变——fcontext
     *          jump 保存/恢复 rbp 绝对指针（lm_ctx_jump_x86_64.S:33/46），
     *          swap_in 仍按 ctx.sp 原偏移 memcpy。
     *          抢占安全：runtime 内全部 mmap 仅 lm_stack_pool.c 一处且
     *          NULL hint，不存在精确抢占该 VA 的来源；两条 syscall 紧邻
     *          无中间操作。munmap 对自有合法映射不会失败；MAP_FIXED 在
     *          刚释放的空闲 VA 不会持续失败，故失败时退避重试/回退仲裁。
     *
     * 不归还 mmap 到池：保留映射换正确性，vm.max_map_count 由部署调参。 */
#ifdef __APPLE__
    if (munmap(co->stack_mmap, co->stack_size) != 0) {
        /* munmap 失败：旧栈完好。逆序恢复 GC 记账（先注册旧栈再注销 buffer），
         * 释放 buffer，回退双重仲裁后放弃。 */
        gc_register_coroutine((char*)base - sizeof(void*),
                              co->stack_mmap, NULL);
        gc_unregister_coroutine((char*)buffer + used);
        free(buffer);
        atomic_store_explicit(&co->swap_state, LM_CO_SWAP_NORMAL,
                              memory_order_release);
        atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
        return 0;
    }
    void* rebuilt;
    for (;;) {
        rebuilt = mmap(co->stack_mmap, co->stack_size,
                       PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        if (rebuilt != MAP_FAILED) break;
        struct timespec backoff = {0, 1000000};   /* 1ms 退避 */
        nanosleep(&backoff, NULL);
    }
#else
    madvise(co->stack_mmap, co->stack_size, MADV_DONTNEED);
#endif
    /* 8. 置 buffer/size/SWAPPED/stealable=0。栈字段保留（mmap 仍在，基址不变），
     *    swap_in 用同 ctx.sp 恢复内容到原偏移。 */
    co->swap_buffer = buffer;
    co->swap_size   = used;
    atomic_store_explicit(&co->stealable, 0, memory_order_release);
    atomic_store_explicit(&co->swap_state, LM_CO_SWAP_SWAPPED,
                          memory_order_release);
    /* 栈仲裁归还：state 回 SUSPENDED（协程仍挂起，内容已在 buffer）。
     * 顺序：先 SWAPPED 后 SUSPENDED——resume 循环先查 swap_state 做 swap_in、
     * 再 CAS state，任意交错都安全。 */
    atomic_store_explicit(&co->state, LM_CO_SUSPENDED, memory_order_release);
    return 1;
#endif /* LM_CTX_FCONTEXT */
}

/* Phase 8.6 D：reaper 入口（sysmon 节流调用）。
 * 持锁遍历注册表：destroy 阻塞于锁，保证遍历期间 co 不被释放（防 UAF）。
 * 单 sysmon 线程扫描，无并发 reap。swap_out 的慢路径（GC+pool）在锁内，
 * stall 可接受（5s 一轮，多数协程廉价跳过非 idle 条件）。
 * 锁序：reaper 持 registry 锁→gc 锁（register/unregister）；无 mutator
 * 反向持 gc 锁→registry 锁，故无死锁。 */
void lm_co_reap_idle(uint64_t now_ns) {
    pthread_mutex_lock(&g_co_reg_lock);
    for (lm_co_t* co = g_co_reg_head; co; co = co->reg_next) {
        co_swap_out(co, now_ns);
    }
    pthread_mutex_unlock(&g_co_reg_lock);
}
