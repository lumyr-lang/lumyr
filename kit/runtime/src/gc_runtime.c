/* gc_runtime.c —— 完整标记-清除 GC 实现（含线程本地分配 TLA）
 *
 * 设计要点：
 *   - 所有 GC 管理的堆块前面统一加 GCObject 头（16 字节）
 *   - 全局链表 g_gc_objects 跟踪所有活跃块（含本地空闲链表中的预分配块）
 *   - gc_alloc 超过阈值时自动触发 gc_collect
 *   - g_in_gc 防止 GC 递归重入
 *   - 全局链表操作加 mutex（线程安全）
 *
 * TLA（Thread-Local Allocation）：
 *   - 小对象（user_size <= TLA_MAX_SIZE）优先从线程本地空闲链表分配（无锁）
 *   - 本地链表空时：先从全局空闲链表批量取用（一次加锁），仍不够则批量 malloc
 *   - 全局空闲链表用 GCObject.next 链接（对象不在 g_gc_objects 中，无冲突）
 *   - 本地空闲链表用用户数据区域前 8 字节存储 next（TLA_LOCAL_NEXT 宏），
 *     不占用 GCObject.next，避免与 g_gc_objects 链表冲突导致全局链表断裂
 *   - 本地空闲链表中的对象保留在 g_gc_objects 中（marked=2 永生），分配无锁
 *   - 全局空闲链表中的对象不在 g_gc_objects 中，取用时空闲→活跃需插入全局链表（已持锁）
 *   - sweep 时小对象不 free，从 g_gc_objects 摘除后移入全局空闲链表
 *   - 大对象（user_size > TLA_MAX_SIZE）走原有直接 malloc + free 路径
 */
#include "gc_runtime.h"
#include "lumyr_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <stdatomic.h>
/* 跨平台高精度计时 */
#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <time.h>
#endif

/* 闭包实例扫描（定义于 ast/func_compile.c）：遍历 InterpFuncPayload.captured_cells，
 * 对每个堆 Value* 单元调用 mark(*cell)。GC 标记 VAL_FUNC 时调用，避免捕获的字符串/数组被误回收。 */
/* 弱定义桩：解释器通道会覆盖此实现；编译通道不调用，空桩避免链接缺失。 */
__attribute__((weak)) void lumyr_interp_scan_captures(const RuntimeFunc* rf, void (*mark)(Value)) {
    (void)rf; (void)mark;
}

/* 暂停生成器 GC 根扫描（定义于 src/ir/vm.c）：遍历所有 yield 暂停的生成器，
 * 标记其 stack/frame 中持有的 GC 对象引用。生成器暂停后不再是当前 VM 根，
 * 若不扫描会导致内部引用被错误回收，内存复用后覆盖生成器字段导致堆破坏。 */
__attribute__((weak)) void lumyr_gc_mark_paused_generators(void) {
}

/* 生成器对象 GC 标记（定义于 src/ir/vm.c）：标记 VAL_GENERATOR 持有的所有引用。
 * gc_mark 遇到 VAL_GENERATOR 时调用此回调，避免生成器的 wrapped_gen/wrap_fn/stack
 * 等引用被错误回收导致堆破坏。 */
__attribute__((weak)) void lumyr_gc_mark_generator(Value v) {
    (void)v;
}
#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    include <sanitizer/asan_interface.h>  /* ASan：保守栈扫描前检测 poisoned 红区 */
#    define GC_ASAN_ENABLED 1
#  endif
#endif
#if !defined(GC_ASAN_ENABLED) && defined(__SANITIZE_ADDRESS__)
#  include <sanitizer/asan_interface.h>
#  define GC_ASAN_ENABLED 1
#endif

/* 编译期断言：GCObject 必须保持 16 字节（user_size 利用原填充空间） */
_Static_assert(sizeof(GCObject) == 16, "GCObject must be 16 bytes");

/* ---- TLA 常量 ---- */
#define TLA_MAX_SIZE   256   /* 用户数据 <=256 字节走 TLA */
#define TLA_BATCH      16    /* 本地链表空时批量分配/取用的对象数 */

/* 指针有效性校验：并发标记时栈扫描可能读到撕裂 Value，跳过明显非法指针 */
#define GC_VALID_PTR(p) ((p) && (unsigned long long)(p) >= 4096 && (unsigned long long)(p) <= 0x00007fffffffffffULL)

/* 线程本地空闲链表（无锁访问）：对象在 g_gc_objects 中，marked=2 */
static _Thread_local GCObject* tla_local_free = NULL;
static _Thread_local int tla_local_count = 0;

/* ---- 全局状态 ---- */
static GCObject* g_gc_objects = NULL;
static size_t g_gc_bytes = 0;
static size_t g_gc_threshold = 64 * 1024 * 1024;  /* 初始阈值 64MB */
static pthread_mutex_t g_gc_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_in_gc = 0;  /* 原子：防止多线程并发触发 GC（CAS 互斥） */
static int g_gc_disable = 0;  /* GC 暂停计数器（构造复合对象时使用） */

/* ---- 分代 GC：全局状态 ----
 * g_gc_generational=1 启用分代 GC（默认），可通过 LUMYR_GC_GENERATIONAL=0 关闭。
 * 新生代：age < PROMOTE_AGE，Minor GC 只 sweep 新生代，存活对象 age++。
 * 老年代：age >= PROMOTE_AGE，仅 Major GC  sweep，大对象（>TLA_MAX_SIZE）直接进入老年代。
 * Remembered Set（老年代→新生代引用追踪）：第一版未实现，Minor GC 采用全量标记简化方案。
 *   未来优化方向：写屏障中追踪老年代对象引用新生代的情况，Minor GC 时只扫描 remembered set
 *   中的老年代对象而非全部老年代，减少标记开销。 */
static int g_gc_generational = 1;
static int g_gc_generational_checked = 0;  /* 是否已读取环境变量 */
static size_t g_young_bytes = 0;     /* 新生代对象总字节数（含 GCObject 头） */
static size_t g_old_bytes = 0;       /* 老年代对象总字节数（含 GCObject 头） */
static size_t g_young_threshold = 8 * 1024 * 1024;   /* 新生代阈值，超过触发 Minor GC */
static size_t g_old_threshold = 64 * 1024 * 1024;    /* 老年代阈值，超过触发 Major GC */
static unsigned long long g_minor_gc_count = 0;  /* Minor GC 次数 */
static unsigned long long g_major_gc_count = 0;  /* Major GC 次数 */

/* ---- Remembered Set（老年代→新生代跨代引用追踪）----
 * 记录所有可能引用新生代对象的老年代对象。Minor GC 时只扫描根 + remembered set
 * 中的老年代对象，而非全部老年代，大幅减少标记开销。
 * 条目在 Minor GC 之间持久化；Major GC sweep 前清空（避免悬空指针）。
 * 用 GCObject.flags 的 GC_OBJ_IN_RS 位去重（每个老年代对象最多一条）。 */
static GCObject** g_remembered_set = NULL;
static size_t g_rs_size = 0;
static size_t g_rs_cap = 0;
static pthread_mutex_t g_rs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- 增量标记：运行时开关 ----
 * 1=启用增量标记（初始STW + 并发标记 + 最终STW），0=全量 STW 标记（fallback）
 * 可通过环境变量 LUMYR_GC_INCREMENTAL=0 关闭 */
static int g_gc_incremental = 1;
static int g_gc_incremental_checked = 0;  /* 是否已读取环境变量 */

/* 并发标记阶段标志：1=写屏障生效 */
volatile int g_gc_marking = 0;

/* ---- 增量标记：标记栈（全局灰色对象队列） ----
 * GC 线程 pop 处理灰色对象，应用线程写屏障 push 新发现的白色对象。
 * 用 g_mark_stack_mutex 保护（并发标记期间无 g_gc_mutex）。
 * 初始容量 1024，2x 动态扩容。 */
static GCObject** g_mark_stack = NULL;
static size_t g_mark_stack_size = 0;
static size_t g_mark_stack_cap = 0;
static pthread_mutex_t g_mark_stack_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ---- STW 计时统计 ---- */
static unsigned long long g_stw_total_ns = 0;  /* 累计 STW 停顿（初始+最终） */

/* ---- 协作式 Stop-The-World ----
 * GC 运行时设置 g_gc_stw，VM 解释循环在每条指令前检查并自旋等待。
 * 编译通道在循环回边/函数调用/builtin/每N条指令前检查。
 *
 * 真正的 STW 确认：每个注册线程的 entry 中有 volatile int at_safepoint。
 * 线程进入 gc_stw_check() 自旋时设置 at_safepoint=1，退出时设置 0。
 * GC 线程轮询所有注册线程直到全部 at_safepoint==1（带超时保护），
 * 确保标记时所有线程栈稳定，彻底消除 torn Value 竞态。 */
volatile int g_gc_stw = 0;

/* 当前线程的根（VM 执行循环注册，供 gc_alloc 自动触发 GC 使用） */
static _Thread_local Value*   tls_stack = NULL;
static _Thread_local int*     tls_sp = NULL;
static _Thread_local StackFrame* tls_frame = NULL;

/* 编译通道（C 代码生成）帧链顶 + 注册标记 */
static _Thread_local CFrame*  tls_cframe = NULL;
static _Thread_local int      tls_cframe_registered = 0;

/* ---- 多线程栈根注册表 ----
 * GC 时遍历所有注册线程，扫描每个线程的操作数栈 + 帧链局部变量。
 * 用 g_gc_mutex 保护（gc_collect 已持有该锁，遍历无需额外加锁）。
 * 支持同一线程嵌套注册（头插，栈式语义），unregister 移除最近的 entry。 */
typedef struct GCThreadEntry {
    pthread_t tid;
    Value* stack;
    int* sp_ptr;
    StackFrame* frame;
    volatile int at_safepoint;  /* STW 确认：线程在 gc_stw_check 自旋时为 1 */
    void* c_stack_top;          /* 线程注册时的栈顶（保守扫描上界） */
    void* c_stack_sp;           /* 最近 safepoint 时的栈指针（保守扫描下界） */
    struct GCThreadEntry* next;
} GCThreadEntry;

static GCThreadEntry* g_gc_threads = NULL;

/* ---- 编译通道 CFrame 线程注册表 ----
 * GC 时遍历所有注册线程，扫描每个线程的 CFrame 链（操作数栈 + 局部变量指针数组）。
 * 用 g_gc_mutex 保护（gc_collect 已持有该锁，遍历无需额外加锁）。
 * cframe_ptr 指向该线程的 tls_cframe 变量地址，GC 时解引用获取当前链顶。 */
typedef struct GCCFrameEntry {
    pthread_t tid;
    CFrame** cframe_ptr;
    volatile int at_safepoint;  /* STW 确认：线程在 gc_stw_check 自旋时为 1 */
    void* c_stack_top;          /* 线程注册时的栈顶（保守扫描上界） */
    void* c_stack_sp;           /* 最近 safepoint 时的栈指针（保守扫描下界） */
    struct GCCFrameEntry* next;
} GCCFrameEntry;

static GCCFrameEntry* g_gc_cframe_threads = NULL;

/* 当前线程的注册 entry 指针（gc_stw_check 用它们设置 at_safepoint） */
static _Thread_local GCThreadEntry* tls_cur_vm_entry = NULL;
static _Thread_local GCCFrameEntry* tls_cur_cf_entry = NULL;

/* 设置当前线程所有注册 entry 的 at_safepoint（VM + CFrame）。
 * 同一线程可能嵌套注册多次（vm_run 递归调用），必须全部标记，
 * 否则 gc_wait_all_threads_at_safepoint 会因旧 entry at_safepoint=0 而永久自旋。 */
static void gc_set_self_at_safepoint(int val) {
    pthread_t self = pthread_self();
    /* 必须持 g_gc_mutex：gc_unregister_thread 在锁内 free(GCThreadEntry)，
     * 不加锁遍历会读到已释放的 entry → UAF。 */
    pthread_mutex_lock(&g_gc_mutex);
    for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->at_safepoint = val;
    }
    for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->at_safepoint = val;
    }
    pthread_mutex_unlock(&g_gc_mutex);
}

/* 协作式 STW 安全点：VM 解释循环每条指令前调用，编译通道每N条指令/循环/调用前调用。
 * 快速路径：无 GC 时直接返回（仅一次 volatile 读），降低频繁检查的开销。
 * GC 运行时设置 at_safepoint=1 后自旋，GC 线程轮询到所有线程 at_safepoint==1 才开始标记。 */
void gc_stw_check(void) {
    if (!g_gc_stw) return;
    /* 记录当前栈指针，供保守式 C 栈扫描 */
    void* sp = __builtin_frame_address(0);
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_gc_mutex);
    for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->c_stack_sp = sp;
    }
    for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->c_stack_sp = sp;
    }
    pthread_mutex_unlock(&g_gc_mutex);
    gc_set_self_at_safepoint(1);
    while (g_gc_stw) { sched_yield(); }
    gc_set_self_at_safepoint(0);
}

/* 原生阻塞区：线程即将进入 pthread_join / mutex_lock / cond_wait / sleep 等
 * 原生阻塞调用，期间不执行 VM 代码、不修改 GC 根，栈稳定。
 * 标记当前线程所有注册 entry 的 at_safepoint=1，使 GC 能立即扫描本线程并继续。 */
void gc_enter_native_block(void) {
    gc_set_self_at_safepoint(1);
}

/* 原生阻塞调用返回，恢复 at_safepoint=0。
 * 先调用 gc_stw_check_fast()：若 GC 仍在进行，保持 at_safepoint=1 自旋等待，
 * GC 结束后再清除 at_safepoint，消除"已离开安全点但 GC 仍在扫描"的竞态窗口。 */
void gc_leave_native_block(void) {
    gc_stw_check_fast();
    gc_set_self_at_safepoint(0);
}

/* GC 发起者（当前线程）在开始标记前记录自身栈帧下界。
 *
 * 背景：其他工作线程在 gc_stw_check 自旋时记录 c_stack_sp，其栈稳定。
 * 但 GC 发起者本身不停在安全点——它从分配路径（gc_alloc/gc_enable）直接进入
 * gc_collect_minor/major，其当前 C 栈比上次 gc_stw_check 记录的 c_stack_sp 更深。
 * 若不更新，保守式 C 栈扫描的下界过高，会漏扫发起者当前帧上的 C 局部 Value
 * （如 val_array 返回后尚未 push 到 VM 操作数栈的临时值 r），导致该值引用的
 * 堆对象被误标记为白色而 sweep → UAF。
 *
 * 调用时机：已持 g_gc_mutex，根扫描之前。记录 __builtin_frame_address(0) 作为
 * 发起者的 c_stack_sp，使保守扫描覆盖 [c_stack_top, 本帧]，包含 val_array/vm_run
 * 等调用者帧，但不进入 GC 自身内部帧（GC 内部不持有用户根）。 */
static void gc_record_self_cstack_sp(void) {
    void* sp = __builtin_frame_address(0);
    pthread_t self = pthread_self();
    for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->c_stack_sp = sp;
    }
    for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
        if (pthread_equal(e->tid, self)) e->c_stack_sp = sp;
    }
}

/* ---- GC 值保护：临时注册 1 元素最小栈，保护 C 栈上的 Value 不被 GC 回收 ----
 * 用于线程退出（已 unregister）时 val_clone 结果值等场景。TLS 保存旧根，不可嵌套。 */
static _Thread_local Value        tls_protect_val;
static _Thread_local int          tls_protect_sp = 0;
static _Thread_local Value*       tls_protect_old_stack = NULL;
static _Thread_local int*         tls_protect_old_sp = NULL;
static _Thread_local StackFrame*  tls_protect_old_frame = NULL;

void gc_protect_push(Value v) {
    /* 保存当前根（pop 时恢复） */
    gc_get_roots(&tls_protect_old_stack, &tls_protect_old_sp, &tls_protect_old_frame);
    /* 设置 1 元素最小保护栈 */
    tls_protect_val = v;
    tls_protect_sp = 1;
    gc_set_roots(&tls_protect_val, &tls_protect_sp, NULL);
    gc_register_thread(&tls_protect_val, &tls_protect_sp, NULL);
    /* 若 GC 已在进行，设置 at_safepoint 并自旋等待其结束（避免新 entry at_safepoint=0 导致 GC 空等） */
    gc_stw_check_fast();
}

void gc_protect_pop(void) {
    gc_unregister_thread();
    gc_set_roots(tls_protect_old_stack, tls_protect_old_sp, tls_protect_old_frame);
}

/* 更新当前 protect entry 的值（cf 返回后用 r 替换初始 dummy）。
 * 无锁、无注册变更，仅更新 tls_protect_val，GC 下次扫描时看到新值。 */
void gc_protect_set(Value v) {
    tls_protect_val = v;
}

/* 标记对象为内部缓冲区（items/buckets 等），保守 C 栈扫描跳过。
 * 内部缓冲区不是用户对象，不应作为根被标记到灰色栈。 */
static inline GCObject* ptr_to_obj_local(void* ptr) {
    return (GCObject*)((char*)ptr - sizeof(GCObject));
}
void gc_mark_internal_buf(void* ptr) {
    if (!ptr) return;
    GCObject* obj = ptr_to_obj_local(ptr);
    obj->flags |= 0x02;  /* bit1 = GC_OBJ_INTERNAL_BUF */
}

/* ---- 内部辅助：用户指针 <-> GCObject ---- */
static inline GCObject* ptr_to_obj(void* ptr) {
    return (GCObject*)((char*)ptr - sizeof(GCObject));
}
static inline void* obj_to_ptr(GCObject* obj) {
    return (void*)((char*)obj + sizeof(GCObject));
}

/* TLA 实际分配大小：用户数据至少 16 字节（确保小对象也能存下未来的内部指针等） */
static inline size_t tla_real_size(size_t size) {
    return (size < 16) ? 16 : size;
}

/* TLA 本地空闲链表 next 指针：存储在用户数据区域前 8 字节（空闲对象用户数据未使用），
 * 不占用 GCObject.next，避免与 g_gc_objects 链表冲突导致全局链表断裂。
 * tla_real_size 保证用户数据 >=16 字节，足够存放指针。 */
#define TLA_LOCAL_NEXT(obj) (*(GCObject**)obj_to_ptr(obj))

/* ---- 前向声明（分代辅助函数定义在 gc_alloc 之后） ---- */
static int gc_generational_enabled(void);
static void gc_recount_bytes(void);
static int gc_need_collect_locked(void);

/* ============================================================
 * 分配
 * ============================================================ */
void* gc_alloc(size_t size, int vtype)
{
    /* 分配前 STW 安全点检查：若 GC 运行中，设置 at_safepoint 并自旋等待。
     * 必须用 gc_stw_check() 而非裸 while(g_gc_stw)，否则 GC 轮询时看不到本线程暂停，造成死锁。 */
    gc_stw_check();
    /* ---- TLA 快速路径：小对象从本地空闲链表分配（无锁） ----
     * 注意：GC 运行期间（g_in_gc）跳过本地空闲链表，强制走加锁路径，
     * 避免无锁修改 marked 位与并发 sweep 产生竞态。 */
    if (size <= TLA_MAX_SIZE && !atomic_load_explicit(&g_in_gc, memory_order_relaxed)) {
        size_t real_sz = tla_real_size(size);
        /* 1. 扫描本地空闲链表，找第一个 user_size >= 请求大小的对象 */
        GCObject* prev = NULL;
        GCObject* cur = tla_local_free;
        while (cur) {
            if (cur->user_size >= real_sz) {
                /* 从本地链表摘除（使用用户数据区的 next 指针，不触碰 GCObject.next） */
                if (prev) TLA_LOCAL_NEXT(prev) = TLA_LOCAL_NEXT(cur);
                else tla_local_free = TLA_LOCAL_NEXT(cur);
                tla_local_count--;
                /* 初始化：对象已在 g_gc_objects 中。
                 * 非标记期：marked=3（新分配预标记，确保首个 GC 周期不被 sweep）。
                 * 并发标记期：marked=1（黑色），新对象不参与本轮回收，也不需要被扫描。
                 * age=0：新分配对象进入新生代（TLA 快速路径不更新分代计数器，漂移在 GC 后 recalculation 修正）。 */
                cur->marked = g_gc_marking ? 1 : 3;
                cur->vtype = (unsigned char)vtype;
                cur->age = 0;
                cur->flags = 0;
                memset(obj_to_ptr(cur), 0, size);
                /* 不增加 g_gc_bytes（对象一直在全局链表中，已被统计） */
                return obj_to_ptr(cur);
            }
            prev = cur;
            cur = TLA_LOCAL_NEXT(cur);
        }

        /* 2. 本地链表未命中：直接 batch malloc。
         * 不从全局空闲链表取用：盲取 16 个混合尺寸对象会污染本地链表，
         * first-fit 让小请求取走栈顶大对象，小对象滞留链尾，本地链表无界膨胀
         * （实测 100M 次扫描）。直接 batch malloc 保持本地链表只含同批次同尺寸对象，
         * 扫描 O(1)。全局空闲链表由 Major GC 清理，不影响正确性。 */

        /* 3. 还是没有，批量 malloc 新对象 */
        size_t total = sizeof(GCObject) + real_sz;
        GCObject* batch[TLA_BATCH];
        for (int i = 0; i < TLA_BATCH; i++) {
            batch[i] = (GCObject*)malloc(total);
            if (!batch[i]) {
                LOG_ERROR("GC: out of memory (requested %zu bytes)\n", size);
                abort();
            }
            batch[i]->marked = 0;
            batch[i]->vtype = 0;
            batch[i]->age = 0;  /* 新对象进入新生代 */
            batch[i]->flags = 0;
            batch[i]->user_size = (uint32_t)real_sz;
            batch[i]->next = NULL;
        }

        /* 预分配块（i>=1）标记为永生（本地空闲链表）；
         * batch[0] 临时设为永生(marked=2)，确保 GC 不会回收这个"未发布"对象，
         * GC 后统一恢复为 marked=3（新分配预标记）或标记期 marked=1。 */
        for (int i = 1; i < TLA_BATCH; i++) batch[i]->marked = 2;
        batch[0]->marked = 2;

        int need_collect_type = 0;  /* 1=minor, 2=major */
        pthread_mutex_lock(&g_gc_mutex);
        for (int i = 0; i < TLA_BATCH; i++) {
            batch[i]->next = g_gc_objects;
            g_gc_objects = batch[i];
            g_gc_bytes += total;
            g_young_bytes += total;  /* 小对象全部进入新生代 */
        }
        need_collect_type = gc_need_collect_locked();
        pthread_mutex_unlock(&g_gc_mutex);

        /* 第 0 个作为返回值（活跃对象） */
        batch[0]->vtype = (unsigned char)vtype;
        memset(obj_to_ptr(batch[0]), 0, size);

        /* 剩余 TLA_BATCH-1 个放入本地空闲链表（marked=2 永生，保留在 g_gc_objects）
         * 使用用户数据区存储 next，不覆盖 g_gc_objects 的链接 */
        for (int i = 1; i < TLA_BATCH; i++) {
            TLA_LOCAL_NEXT(batch[i]) = tla_local_free;
            tla_local_free = batch[i];
            tla_local_count++;
        }

        if (need_collect_type && tls_stack && tls_sp && (tls_frame || tls_cframe)) {
            if (need_collect_type == 1) gc_collect_minor(tls_stack, *tls_sp, tls_frame);
            else gc_collect_major(tls_stack, *tls_sp, tls_frame);
        }
        /* 无论是否触发 GC，都要把 batch[0] 从临时永生态恢复为新分配状态，
         * 否则它会永远停留在 marked=2 永生态，下次 GC 永不回收。 */
        batch[0]->marked = g_gc_marking ? 1 : 3;
        return obj_to_ptr(batch[0]);
    }

    /* ---- 大对象：原有直接 malloc + 插入链表路径 ----
     * 注意：此路径也处理 g_in_gc 期间的小对象分配（TLA 快速路径被跳过）。
     * 大对象（>TLA_MAX_SIZE）直接进入老年代（age=PROMOTE_AGE），
     * 因为大对象在新生代存活时间通常较长，直接晋升避免频繁拷贝/扫描。 */
    size_t total = sizeof(GCObject) + size;
    GCObject* obj = (GCObject*)malloc(total);
    if (!obj) {
        LOG_ERROR("GC: out of memory (requested %zu bytes)\n", size);
        abort();
    }
    obj->marked = g_gc_marking ? 1 : 3;  /* 新分配预标记 / 标记期黑色 */
    obj->vtype = (unsigned char)vtype;
    obj->age = (size > TLA_MAX_SIZE) ? PROMOTE_AGE : 0;  /* 大对象直接老年代 */
    obj->flags = 0;
    obj->user_size = (uint32_t)size;
    obj->next = NULL;
    memset(obj_to_ptr(obj), 0, size);

    int need_collect_type = 0;  /* 1=minor, 2=major */
    pthread_mutex_lock(&g_gc_mutex);
    obj->next = g_gc_objects;
    g_gc_objects = obj;
    g_gc_bytes += total;
    if (obj->age >= PROMOTE_AGE) g_old_bytes += total;
    else g_young_bytes += total;
    need_collect_type = gc_need_collect_locked();
    pthread_mutex_unlock(&g_gc_mutex);

    if (need_collect_type && tls_stack && tls_sp && (tls_frame || tls_cframe)) {
        if (need_collect_type == 1) gc_collect_minor(tls_stack, *tls_sp, tls_frame);
        else gc_collect_major(tls_stack, *tls_sp, tls_frame);
    }
    return obj_to_ptr(obj);
}

/* ============================================================
 * 重分配（仅用于内部缓冲区：array items 等）
 * 改用 malloc + memcpy，旧对象保留在链表中不释放。
 *
 * 为什么不用 realloc：realloc 可能移动内存并释放旧块，但调用方
 * （如 lumyr_array_add）在锁外更新 items 指针。在 gc_realloc 返回
 * 和调用方更新指针之间，另一个线程的 GC 可能扫描到已释放的旧指针，
 * 导致 gc_mark_ptr 读取已释放内存 → segfault。
 *
 * 改用 malloc + memcpy 后：旧对象保留在 g_gc_objects 中，内存不被
 * 释放。调用方更新指针后，旧对象不再被引用，下一次 GC sweep 回收它。
 * ============================================================ */
void* gc_realloc(void* ptr, size_t new_size)
{
    if (!ptr) return gc_alloc(new_size, VAL_ARRAY);

    GCObject* old_obj = ptr_to_obj(ptr);
    unsigned char vtype = old_obj->vtype;
    size_t old_user = old_obj->user_size;
    size_t copy_size = old_user < new_size ? old_user : new_size;

    /* 分配新对象 + 复制（不使用 realloc，避免旧块被提前释放） */
    size_t new_total = sizeof(GCObject) + new_size;
    GCObject* new_obj = (GCObject*)malloc(new_total);
    if (!new_obj) {
        LOG_ERROR("GC: realloc out of memory (requested %zu bytes)\n", new_size);
        abort();
    }
    memcpy(new_obj, old_obj, sizeof(GCObject) + copy_size);
    new_obj->user_size = (uint32_t)new_size;
    /* 标记期：新内部缓冲区设黑色（不被本轮 sweep）；非标记期：预标记（存活首轮 GC）。
     * 关键：数组/map 增长时更新 items/buckets 指针不触发写屏障，若父对象已黑色
     * 而新缓冲区为白色，并发标记期间会被漏标 → sweep 回收 → 悬空指针。 */
    new_obj->marked = g_gc_marking ? 1 : 3;
    new_obj->vtype = vtype;
    new_obj->age = PROMOTE_AGE;  /* 内部缓冲区始终老年代，避免被老年代容器引用时 Minor GC 错误回收 */
    new_obj->flags = 0;
    new_obj->next = NULL;
    if (new_size > copy_size) {
        memset((char*)new_obj + sizeof(GCObject) + copy_size, 0, new_size - copy_size);
    }

    pthread_mutex_lock(&g_gc_mutex);
    /* 新对象插入全局链表 */
    new_obj->next = g_gc_objects;
    g_gc_objects = new_obj;
    g_gc_bytes += new_total;
    /* 内部缓冲区始终老年代（gc_realloc 仅用于内部缓冲区） */
    g_old_bytes += new_total;
    /* 旧对象保留在链表中，marked 保持原值（通常为 0）。
     * 调用方更新指针后，旧对象不再被引用，下一次 GC sweep 释放它。
     * 在调用方更新指针前，并发 GC 可安全扫描旧对象（内存未被释放）。 */
    pthread_mutex_unlock(&g_gc_mutex);

    return obj_to_ptr(new_obj);
}

/* ============================================================
 * 老年代分配：内部缓冲区（items/buckets/tree/MapEntry）直接进入老年代
 * ============================================================ */
void* gc_alloc_old(size_t size, int vtype)
{
    void* ptr = gc_alloc(size, vtype);
    GCObject* obj = ptr_to_obj(ptr);
    if (obj->age < PROMOTE_AGE) {
        obj->age = PROMOTE_AGE;
    }
    return ptr;
}

/* ============================================================
 * Remembered Set 操作
 * ============================================================ */

/* 将老年代对象加入 remembered set（去重：已有 GC_OBJ_IN_RS 位则跳过）。
 * 调用方无需加锁，内部加 g_rs_mutex。 */
static void remembered_set_add(GCObject* obj)
{
    if (!obj || obj->age < PROMOTE_AGE) return;  /* 只记录老年代对象 */
    if (obj->flags & GC_OBJ_IN_RS) return;  /* 已在 rs 中，去重 */

    pthread_mutex_lock(&g_rs_mutex);
    /* 双重检查：加锁后可能已被其他线程加入 */
    if (obj->flags & GC_OBJ_IN_RS) {
        pthread_mutex_unlock(&g_rs_mutex);
        return;
    }
    if (g_rs_size >= g_rs_cap) {
        size_t new_cap = g_rs_cap ? g_rs_cap * 2 : 64;
        GCObject** new_rs = (GCObject**)realloc(g_remembered_set, new_cap * sizeof(GCObject*));
        if (!new_rs) {
            LOG_ERROR("GC: out of memory expanding remembered set\n");
            abort();
        }
        g_remembered_set = new_rs;
        g_rs_cap = new_cap;
    }
    obj->flags |= GC_OBJ_IN_RS;
    g_remembered_set[g_rs_size++] = obj;
    pthread_mutex_unlock(&g_rs_mutex);
}

/* 清空 remembered set：清除所有对象的 GC_OBJ_IN_RS 位，重置 size=0。
 * 必须在 Major GC sweep 之前调用（此时所有对象仍有效，避免悬空指针）。 */
static void remembered_set_clear(void)
{
    pthread_mutex_lock(&g_rs_mutex);
    for (size_t i = 0; i < g_rs_size; i++) {
        if (g_remembered_set[i]) {
            g_remembered_set[i]->flags &= ~GC_OBJ_IN_RS;
        }
    }
    g_rs_size = 0;
    pthread_mutex_unlock(&g_rs_mutex);
}

/* 暴露 remembered set 大小（统计用） */
size_t gc_rs_size(void) { return g_rs_size; }

/* ============================================================
 * Remembered Set 写屏障检查
 *
 * 在所有修改堆对象内部引用的位置调用。如果 owner 是老年代容器且
 * new_val 可能引用新生代对象，将 owner 加入 remembered set。
 * ============================================================ */
void gc_remembered_set_check(Value owner, Value new_val)
{
    if (!gc_generational_enabled()) return;

    /* 从 owner 获取 GCObject*（仅 VAL_ARRAY/VAL_MAP 且非 stack_alloc） */
    GCObject* owner_obj = NULL;
    if (owner.type == VAL_ARRAY && GC_VALID_PTR(owner.v.array) && !owner.v.array->stack_alloc) {
        owner_obj = ptr_to_obj(owner.v.array);
    } else if (owner.type == VAL_MAP && GC_VALID_PTR(owner.v.map) && !owner.v.map->stack_alloc) {
        owner_obj = ptr_to_obj(owner.v.map);
    } else {
        return;  /* stack_alloc 容器在 C 栈上，等效于根，无需 rs */
    }

    /* owner 不是老年代，无需追踪 */
    if (owner_obj->age < PROMOTE_AGE) return;

    /* 有效性校验：防止已释放/损坏的容器被加入 remembered set。
     * VM 解释器中弹出栈到 C 局部变量的容器在扩容分配触发 GC 时可能被回收，
     * 此处校验内部指针对齐性和字段合理性，跳过可疑对象。 */
    if (owner.type == VAL_ARRAY) {
        ValueArray* arr = owner.v.array;
        if (arr->items && (!GC_VALID_PTR(arr->items) || ((unsigned long long)arr->items & 0xF) != 0))
            return;
        if (arr->len < 0 || arr->cap < 0 || arr->len > arr->cap) return;
    } else {
        ValueMap* m = owner.v.map;
        if (m->buckets && (!GC_VALID_PTR(m->buckets) || ((unsigned long long)m->buckets & 0xF) != 0))
            return;
        if (m->cap <= 0) return;
    }

    /* 检查 new_val 是否可能引用新生代对象 */
    int may_ref_young = 0;
    switch (new_val.type) {
    case VAL_STRING:
        if (!new_val.str_inline && GC_VALID_PTR(new_val.v.s)) {
            if (ptr_to_obj(new_val.v.s)->age < PROMOTE_AGE) may_ref_young = 1;
        }
        break;
    case VAL_ARRAY:
        if (GC_VALID_PTR(new_val.v.array) && !new_val.v.array->stack_alloc) {
            if (ptr_to_obj(new_val.v.array)->age < PROMOTE_AGE) may_ref_young = 1;
        }
        break;
    case VAL_MAP:
        if (GC_VALID_PTR(new_val.v.map) && !new_val.v.map->stack_alloc) {
            if (ptr_to_obj(new_val.v.map)->age < PROMOTE_AGE) may_ref_young = 1;
        }
        break;
    case VAL_FUNC:
        /* 保守：captures 可能引用新生代对象 */
        may_ref_young = 1;
        break;
    case VAL_ERROR:
        /* 保守：type/message/stack 字符串可能是新生代 */
        may_ref_young = 1;
        break;
    default:
        /* INT/DOUBLE/BOOL/CHAR/BYTE/NONE：无堆引用 */
        break;
    }

    if (may_ref_young) {
        remembered_set_add(owner_obj);
    }
}

/* ============================================================
 * 标记原始 GC 指针（buckets/tree/MapEntry 等内部缓冲区）
 * 不递归 Value，仅标记该 GCObject 不被 sweep
 * ============================================================ */
int gc_mark_ptr(void* ptr)
{
    if (!ptr) return 0;
    GCObject* obj = ptr_to_obj(ptr);
    /* marked=1 已标记；marked=2 永生（钉住/空闲链表）；
     * marked=3 新分配预标记（需遍历，视同未标记） */
    if (obj->marked == 1 || obj->marked == 2) return 0;
    obj->marked = 1;
    return 1;
}

/* 钉住对象：marked=2 表示永生，sweep 永不回收，mark 跳过 */
void gc_pin(void* ptr)
{
    if (!ptr) return;
    GCObject* obj = ptr_to_obj(ptr);
    obj->marked = 2;
}

/* ============================================================
 * 标记 Value：堆类型标记其 GCObject + 递归标记内部引用
 *
 * 指针校验：并发标记时，GC 基于 sp 快照扫描栈，线程可能在扫描期间
 * pop 槽位再 push 新值覆盖，导致 GC 读到撕裂 Value（type 合法但 pointer
 * 为旧 int 值或垃圾）。此处校验指针范围，跳过明显非法的指针，防止
 * gc_mark_ptr 访问非法地址导致 segfault。
 * ============================================================ */
void gc_mark(Value v)
{
    switch (v.type) {
    case VAL_STRING: {
        if (!v.str_inline && GC_VALID_PTR(v.v.s)) gc_mark_ptr(v.v.s);
        break;
    }
    case VAL_ARRAY: {
        if (!GC_VALID_PTR(v.v.array)) break;
        if (v.v.array->stack_alloc) {
            if (v.v.array->items) {
                if (!v.v.array->items_stack_alloc) {
                    gc_mark_ptr(v.v.array->items);
                }
                for (int i = 0; i < v.v.array->len; i++) {
                    gc_mark(v.v.array->items[i]);
                }
            }
        } else {
            if (!gc_mark_ptr(v.v.array)) break;
            if (v.v.array->items) {
                gc_mark_ptr(v.v.array->items);
                for (int i = 0; i < v.v.array->len; i++) {
                    gc_mark(v.v.array->items[i]);
                }
            }
        }
        break;
    }
    case VAL_MAP: {
        ValueMap* m = v.v.map;
        if (!GC_VALID_PTR(m)) break;
        if (m->stack_alloc) {
            /* 栈分配 ValueMap：无 GCObject 头，只标记内部缓冲区和递归键值 */
        } else {
            if (!gc_mark_ptr(m)) break;
        }
        if (m->buckets) gc_mark_ptr(m->buckets);
        if (m->tree) gc_mark_ptr(m->tree);
        for (int i = 0; i < m->cap; i++) {
            MapEntry* e = m->buckets[i];
            if (!e) continue;
            if (m->tree[i]) {
                MapEntry* stk[256];
                int top = 0;
                MapEntry* cur = e;
                while (cur || top > 0) {
                    while (cur) {
                        if (top < 256) stk[top++] = cur;
                        cur = cur->left;
                    }
                    if (top == 0) break;
                    cur = stk[--top];
                    gc_mark_ptr(cur);
                    gc_mark(cur->key);
                    gc_mark(cur->value);
                    cur = cur->right;
                }
            } else {
                while (e) {
                    gc_mark_ptr(e);
                    gc_mark(e->key);
                    gc_mark(e->value);
                    e = e->next;
                }
            }
        }
        break;
    }
    case VAL_ERROR: {
        if (v.v.err.type) gc_mark_ptr(v.v.err.type);
        if (v.v.err.message) gc_mark_ptr(v.v.err.message);
        if (v.v.err.stack) gc_mark_ptr(v.v.err.stack);
        break;
    }
    case VAL_FUNC: {
        RuntimeFunc* f = v.v.func.func_obj;
        if (f && GC_VALID_PTR(f) && f->captures && GC_VALID_PTR(f->captures) && f->capture_count > 0) {
            for (int i = 0; i < f->capture_count; i++) {
                gc_mark(f->captures[i]);
            }
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -1) {
            /* VM 解释器闭包实例：capture_count==-1，captures 指向 InterpFuncPayload，
             * 其中 captured_cells[i] 为堆 Value* 单元，需递归标记其内容 */
            lumyr_interp_scan_captures(f, gc_mark);
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -2 &&
                   f->captures && GC_VALID_PTR(f->captures)) {
            /* 编译通道闭包：capture_count==-2，captures 是 Value** cell 指针数组，
             * 末尾以 NULL 哨兵结束；每个 cell 指向 malloc 的 Value，标记其内容 */
            Value** caps = (Value**)f->captures;
            for (int i = 0; caps[i] != NULL; i++) {
                if (GC_VALID_PTR(caps[i])) gc_mark(*caps[i]);
            }
        }
        break;
    }
    case VAL_GENERATOR: {
        /* 标记生成器持有的所有引用（wrapped_gen/wrap_fn/stack/frame 等），
         * 避免 GC 错误回收导致堆破坏 */
        lumyr_gc_mark_generator(v);
        break;
    }
    default:
        break;
    }
}

/* ============================================================
 * 标记根：VM 栈 + 当前帧及父帧链的局部变量
 * ============================================================ */
void gc_mark_roots(Value* stack, int sp, StackFrame* frame)
{
    if (stack) {
        for (int i = 0; i < sp; i++) {
            gc_mark(stack[i]);
        }
    }
    StackFrame* f = frame;
    while (f) {
        if (f->vals) {
            for (int i = 0; i < f->cnt; i++) {
                gc_mark(f->vals[i]);
            }
        }
        f = f->parent;
    }
    /* 标记所有暂停生成器持有的 GC 对象（yield 后不再是当前 VM 根） */
    lumyr_gc_mark_paused_generators();
}

/* ============================================================
 * 标记栈操作（全局灰色对象队列）
 * ============================================================ */

/* 入栈：设为灰色（marked=4），动态扩容（2x，初始 1024）
 * 调用方必须持有 g_mark_stack_mutex */
static void mark_stack_push_locked(GCObject* obj) {
    if (g_mark_stack_size >= g_mark_stack_cap) {
        size_t new_cap = g_mark_stack_cap ? g_mark_stack_cap * 2 : 1024;
        GCObject** new_stack = (GCObject**)realloc(g_mark_stack, new_cap * sizeof(GCObject*));
        if (!new_stack) {
            LOG_ERROR("GC: out of memory expanding mark stack\n");
            abort();
        }
        g_mark_stack = new_stack;
        g_mark_stack_cap = new_cap;
    }
    obj->marked = 4;  /* 灰色 */
    g_mark_stack[g_mark_stack_size++] = obj;
}

/* 出栈：返回 NULL 表示空。不修改 marked（由调用方设为黑色）
 * 调用方必须持有 g_mark_stack_mutex */
static GCObject* mark_stack_pop_locked(void) {
    if (g_mark_stack_size == 0) return NULL;
    return g_mark_stack[--g_mark_stack_size];
}

/* 带锁的 push（供写屏障等外部调用） */
static void mark_stack_push(GCObject* obj) {
    pthread_mutex_lock(&g_mark_stack_mutex);
    mark_stack_push_locked(obj);
    pthread_mutex_unlock(&g_mark_stack_mutex);
}

/* ============================================================
 * 迭代式三色标记：gc_mark_ptr_to_stack / gc_mark_value_to_stack / gc_mark_one
 * ============================================================ */

/* 将白色/预标记对象变灰入标记栈。
 * 返回 1=新入栈，0=已处理(黑色/灰色)/永生/空。
 * marked==1(黑色)、2(永生)、4(已灰色)都返回 0。 */
int gc_mark_ptr_to_stack(void* ptr)
{
    if (!ptr) return 0;
    GCObject* obj = ptr_to_obj(ptr);
    unsigned char m = obj->marked;
    if (m == 1 || m == 2 || m == 4) return 0;
    /* m==0(白色) 或 m==3(新对象预标记，视同未标记） */
    mark_stack_push(obj);
    return 1;
}

/* 标记内部缓冲区（buckets/tree/MapEntry 等）：直接设为黑色，不入灰色栈。
 * 这些对象与 ValueMap 共用 vtype=VAL_MAP，入灰色栈会导致 gc_mark_one 误判。
 * 它们的 Value 子引用（如 MapEntry.key/value）由父级 ValueMap 的 gc_mark_one 显式扫描。 */
static void gc_mark_internal_black(void* ptr)
{
    if (!ptr) return;
    GCObject* obj = ptr_to_obj(ptr);
    if (obj->marked != 2) obj->marked = 1;  /* 永生保持 2，其余设黑色 */
}

/* 对 Value 中的堆对象调用 gc_mark_ptr_to_stack。
 * stack_alloc 容器（无 GCObject 头）直接扫描子元素，不标记自身。 */
void gc_mark_value_to_stack(Value v)
{
    switch (v.type) {
    case VAL_STRING: {
        if (!v.str_inline && GC_VALID_PTR(v.v.s)) {
            gc_mark_ptr_to_stack(v.v.s);
        }
        break;
    }
    case VAL_ARRAY: {
        if (!GC_VALID_PTR(v.v.array)) break;
        ValueArray* arr = v.v.array;
        if (arr->stack_alloc) {
            /* 栈分配 ValueArray：无 GCObject 头，不标记自身；
             * items 若堆分配则标记，然后递归扫描元素 */
            if (arr->items) {
                if (!arr->items_stack_alloc) {
                    gc_mark_ptr_to_stack(arr->items);
                }
                for (int i = 0; i < arr->len; i++) {
                    gc_mark_value_to_stack(arr->items[i]);
                }
            }
        } else {
            /* 堆分配 ValueArray：标记自身（变灰入栈），子元素由 gc_mark_one 扫描 */
            if (!gc_mark_ptr_to_stack(arr)) break;  /* 已黑色/灰色/永生，无需处理 */
            /* items 缓冲区是内部缓冲区，直接标记黑色（不入灰色栈） */
            if (arr->items && !arr->items_stack_alloc) {
                gc_mark_internal_black(arr->items);
            }
        }
        break;
    }
    case VAL_MAP: {
        ValueMap* m = v.v.map;
        if (!GC_VALID_PTR(m)) break;
        if (m->stack_alloc) {
            /* 栈分配 ValueMap：无 GCObject 头，buckets/tree/MapEntry 均为堆分配需标记 */
            if (m->buckets) gc_mark_internal_black(m->buckets);
            if (m->tree) gc_mark_internal_black(m->tree);
            for (int i = 0; i < m->cap; i++) {
                MapEntry* e = m->buckets[i];
                if (!e) continue;
                if (m->tree[i]) {
                    /* 红黑树非递归中序遍历 */
                    MapEntry* stk[256];
                    int top = 0;
                    MapEntry* cur = e;
                    while (cur || top > 0) {
                        while (cur) {
                            if (top < 256) stk[top++] = cur;
                            cur = cur->left;
                        }
                        if (top == 0) break;
                        cur = stk[--top];
                        gc_mark_internal_black(cur);  /* MapEntry 标记黑色 */
                        gc_mark_value_to_stack(cur->key);
                        gc_mark_value_to_stack(cur->value);
                        cur = cur->right;
                    }
                } else {
                    while (e) {
                        gc_mark_internal_black(e);  /* MapEntry 标记黑色 */
                        gc_mark_value_to_stack(e->key);
                        gc_mark_value_to_stack(e->value);
                        e = e->next;
                    }
                }
            }
        } else {
            /* 堆分配 ValueMap：标记自身（变灰入栈），buckets/tree 内部缓冲区直接标记黑色 */
            if (!gc_mark_ptr_to_stack(m)) break;
            if (m->buckets) gc_mark_internal_black(m->buckets);
            if (m->tree) gc_mark_internal_black(m->tree);
        }
        break;
    }
    case VAL_ERROR: {
        /* ValueError 内联在 Value 中，type/message/stack 是堆字符串 */
        if (v.v.err.type) gc_mark_ptr_to_stack(v.v.err.type);
        if (v.v.err.message) gc_mark_ptr_to_stack(v.v.err.message);
        if (v.v.err.stack) gc_mark_ptr_to_stack(v.v.err.stack);
        break;
    }
    case VAL_FUNC: {
        /* RuntimeFunc 是 malloc 不是 GC 对象，不能入栈。
         * 立即遍历 captures（capture_count > 0 时是真实 Value 数组）。
         * capture_count==-1 是解释器 payload 标记：普通函数无捕获；闭包实例的
         * InterpFuncPayload 中 captured_cells[i] 是堆 Value* 单元，需递归标记。 */
        RuntimeFunc* f = v.v.func.func_obj;
        if (f && GC_VALID_PTR(f) && f->captures && GC_VALID_PTR(f->captures) && f->capture_count > 0) {
            for (int i = 0; i < f->capture_count; i++) {
                gc_mark_value_to_stack(f->captures[i]);
            }
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -1) {
            lumyr_interp_scan_captures(f, gc_mark_value_to_stack);
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -2 &&
                   f->captures && GC_VALID_PTR(f->captures)) {
            Value** caps = (Value**)f->captures;
            for (int i = 0; caps[i] != NULL; i++) {
                if (GC_VALID_PTR(caps[i])) gc_mark_value_to_stack(*caps[i]);
            }
        }
        break;
    }
    default:
        /* INT/DOUBLE/BOOL/CHAR/BYTE/NONE：无堆引用 */
        break;
    }
}

/* 处理一个灰色对象的子对象（白色子对象变灰入栈），完成后自身设为黑色。
 * 调用方从标记栈 pop 出对象后调用。
 *
 * 注意：内部缓冲区（buckets/tree/MapEntry/items）不会出现在标记栈中，
 * 它们被 gc_mark_internal_black 直接标记为黑色。因此 gc_mark_one 只需处理
 * "真实"对象：VAL_STRING、VAL_ARRAY(ValueArray)、VAL_MAP(ValueMap)。 */
void gc_mark_one(GCObject* obj)
{
    switch (obj->vtype) {
    case VAL_STRING:
        /* 字符串数据是用户数据区域，不是独立 GC 引用，无子对象 */
        break;
    case VAL_ARRAY: {
        /* 用户数据是 ValueArray 结构体（不是指针） */
        ValueArray* arr = (ValueArray*)obj_to_ptr(obj);
        /* 有效性校验：防止已释放/损坏对象的 items 指针导致崩溃 */
        if (arr->items && (!GC_VALID_PTR(arr->items) || ((unsigned long long)arr->items & 0xF) != 0))
            break;
        if (arr->len < 0 || arr->cap < 0 || arr->len > arr->cap) break;
        if (arr->items) {
            if (!arr->items_stack_alloc) {
                gc_mark_internal_black(arr->items);  /* items 内部缓冲区标记黑色 */
            }
            for (int i = 0; i < arr->len; i++) {
                gc_mark_value_to_stack(arr->items[i]);
            }
        }
        break;
    }
    case VAL_MAP: {
        /* 用户数据是 ValueMap 结构体。
         * 遍历所有 buckets 的所有 entry（链表和红黑树两种形态），
         * 对每个 entry 本身标记黑色（内部缓冲区，不入灰色栈），
         * 再对 entry 的 key 和 value 调用 gc_mark_value_to_stack。
         * 注意：gc_mark_value_to_stack 只标记 buckets/tree 数组本身，
         * 不标记单个 MapEntry，必须在这里显式标记，否则 MapEntry 会被
         * Major GC sweep 回收，导致 buckets 悬空指针。 */
        ValueMap* m = (ValueMap*)obj_to_ptr(obj);
        /* 有效性校验：防止已释放/损坏对象的 buckets 指针导致崩溃 */
        if (m->buckets && (!GC_VALID_PTR(m->buckets) || ((unsigned long long)m->buckets & 0xF) != 0))
            break;
        if (m->cap <= 0) break;
        /* buckets/tree 内部缓冲区标记黑色（防御性：gc_mark_value_to_stack 应已标记） */
        if (m->buckets) gc_mark_internal_black(m->buckets);
        if (m->tree) gc_mark_internal_black(m->tree);
        for (int i = 0; i < m->cap; i++) {
            MapEntry* e = m->buckets[i];
            if (!e) continue;
            if (m->tree[i]) {
                /* 红黑树非递归中序遍历 */
                MapEntry* stk[256];
                int top = 0;
                MapEntry* cur = e;
                while (cur || top > 0) {
                    while (cur) {
                        if (top < 256) stk[top++] = cur;
                        cur = cur->left;
                    }
                    if (top == 0) break;
                    cur = stk[--top];
                    gc_mark_internal_black(cur);  /* MapEntry 内部缓冲区，标记黑色 */
                    gc_mark_value_to_stack(cur->key);
                    gc_mark_value_to_stack(cur->value);
                    cur = cur->right;
                }
            } else {
                while (e) {
                    gc_mark_internal_black(e);  /* MapEntry 内部缓冲区，标记黑色 */
                    gc_mark_value_to_stack(e->key);
                    gc_mark_value_to_stack(e->value);
                    e = e->next;
                }
            }
        }
        break;
    }
    case VAL_ERROR:
        /* VAL_ERROR 不是通过 gc_alloc 分配的独立对象，错误对象内联在 Value 中。
         * gc_mark_one 不会遇到 vtype==VAL_ERROR 的 GCObject。 */
        break;
    case VAL_FUNC:
        /* RuntimeFunc 是 malloc 的，不是 GCObject。gc_mark_one 不会遇到。 */
        break;
    default:
        break;
    }
    /* 处理完成，设为黑色 */
    obj->marked = 1;
}

/* ============================================================
 * Minor GC 专用标记：只标记新生代对象
 *
 * 与全量标记的区别：老年代对象不推入标记栈（它们如果引用新生代，
 * 应该在 remembered set 中，由 gc_scan_remembered_set 扫描）。
 * ============================================================ */

/* 对 Value 中的新生代堆对象调用 gc_mark_ptr_to_stack。
 * 老年代对象不推入栈（由 remembered set 机制追踪）。
 * stack_alloc 容器直接扫描子元素。 */
static void gc_mark_value_to_stack_minor(Value v)
{
    switch (v.type) {
    case VAL_STRING: {
        if (!v.str_inline && GC_VALID_PTR(v.v.s)) {
            /* 根上的字符串无论年代都标记（Major GC 清空 RS 后老年代字符串仍需存活） */
            gc_mark_ptr_to_stack(v.v.s);
        }
        break;
    }
    case VAL_ARRAY: {
        if (!GC_VALID_PTR(v.v.array)) break;
        ValueArray* arr = v.v.array;
        if (arr->stack_alloc) {
            /* 栈分配 ValueArray：无 GCObject 头，直接扫描子元素 */
            if (arr->items) {
                if (!arr->items_stack_alloc) {
                    gc_mark_internal_black(arr->items);
                }
                for (int i = 0; i < arr->len; i++) {
                    gc_mark_value_to_stack_minor(arr->items[i]);
                }
            }
        } else {
            /* 堆分配 ValueArray：无论新生代还是老年代都推入灰色栈。
             * 之前老年代不推入栈（假设由 remembered set 追踪），但 Major GC 会清空
             * remembered set，清空后老年代对象不在 RS 中也不被根扫描覆盖 → 其子对象
             * （items 缓冲区）漏标 → Minor GC sweep 误释放 → UAF。
             * C 栈/VM 栈上的引用是根，必须标记，不论年代。 */
            GCObject* obj = ptr_to_obj(arr);
            (void)obj;
            if (gc_mark_ptr_to_stack(arr)) {
                if (arr->items && !arr->items_stack_alloc) {
                    gc_mark_internal_black(arr->items);
                }
            }
        }
        break;
    }
    case VAL_MAP: {
        ValueMap* m = v.v.map;
        if (!GC_VALID_PTR(m)) break;
        if (m->stack_alloc) {
            /* 栈分配 ValueMap：buckets/tree/MapEntry 均为堆分配需标记黑色 */
            if (m->buckets) gc_mark_internal_black(m->buckets);
            if (m->tree) gc_mark_internal_black(m->tree);
            for (int i = 0; i < m->cap; i++) {
                MapEntry* e = m->buckets[i];
                if (!e) continue;
                if (m->tree[i]) {
                    MapEntry* stk[256];
                    int top = 0;
                    MapEntry* cur = e;
                    while (cur || top > 0) {
                        while (cur) {
                            if (top < 256) stk[top++] = cur;
                            cur = cur->left;
                        }
                        if (top == 0) break;
                        cur = stk[--top];
                        gc_mark_internal_black(cur);  /* MapEntry 标记黑色 */
                        gc_mark_value_to_stack_minor(cur->key);
                        gc_mark_value_to_stack_minor(cur->value);
                        cur = cur->right;
                    }
                } else {
                    while (e) {
                        gc_mark_internal_black(e);  /* MapEntry 标记黑色 */
                        gc_mark_value_to_stack_minor(e->key);
                        gc_mark_value_to_stack_minor(e->value);
                        e = e->next;
                    }
                }
            }
        } else {
            /* 堆分配 ValueMap：无论年代都推入灰色栈（根上的引用必须标记） */
            if (gc_mark_ptr_to_stack(m)) {
                if (m->buckets) gc_mark_internal_black(m->buckets);
                if (m->tree) gc_mark_internal_black(m->tree);
            }
        }
        break;
    }
    case VAL_ERROR: {
        /* ValueError 内联在 Value 中，type/message/stack 是堆字符串 */
        if (v.v.err.type && GC_VALID_PTR(v.v.err.type)) {
            gc_mark_ptr_to_stack(v.v.err.type);
        }
        if (v.v.err.message && GC_VALID_PTR(v.v.err.message)) {
            gc_mark_ptr_to_stack(v.v.err.message);
        }
        if (v.v.err.stack && GC_VALID_PTR(v.v.err.stack)) {
            gc_mark_ptr_to_stack(v.v.err.stack);
        }
        break;
    }
    case VAL_FUNC: {
        /* RuntimeFunc 是 malloc 非 GC 对象，始终遍历 captures。
         * 安全检查：f 和 f->captures 都必须在合理堆范围内，
         * 防止 C 栈上的垃圾 Value 被误判为 VAL_FUNC 导致 UAF。 */
        RuntimeFunc* f = v.v.func.func_obj;
        if (f && GC_VALID_PTR(f) && f->captures && GC_VALID_PTR(f->captures) && f->capture_count > 0) {
            for (int i = 0; i < f->capture_count; i++) {
                gc_mark_value_to_stack_minor(f->captures[i]);
            }
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -1) {
            lumyr_interp_scan_captures(f, gc_mark_value_to_stack_minor);
        } else if (f && GC_VALID_PTR(f) && f->capture_count == -2 &&
                   f->captures && GC_VALID_PTR(f->captures)) {
            Value** caps = (Value**)f->captures;
            for (int i = 0; caps[i] != NULL; i++) {
                if (GC_VALID_PTR(caps[i])) gc_mark_value_to_stack_minor(*caps[i]);
            }
        }
        break;
    }
    default:
        break;
    }
}

/* 处理一个新生代灰色对象的子对象（只将新生代子对象变灰入栈），完成后自身设为黑色。
 * 内部缓冲区（items/buckets/tree/MapEntry）标记黑色：若为老年代则不被 Minor GC sweep，
 * 若为新生代则防止被错误回收（items 可能在数组增长时重新分配，旧缓冲区可能是新生代）。 */
static void gc_mark_one_minor(GCObject* obj)
{
    switch (obj->vtype) {
    case VAL_STRING:
        break;
    case VAL_ARRAY: {
        ValueArray* arr = (ValueArray*)obj_to_ptr(obj);
        if (arr->items && (!GC_VALID_PTR(arr->items) || ((unsigned long long)arr->items & 0xF) != 0))
            break;
        if (arr->len < 0 || arr->cap < 0 || arr->len > arr->cap) break;
        if (arr->items) {
            if (!arr->items_stack_alloc) {
                gc_mark_internal_black(arr->items);  /* items 内部缓冲区标记黑色 */
            }
            for (int i = 0; i < arr->len; i++) {
                gc_mark_value_to_stack_minor(arr->items[i]);
            }
        }
        break;
    }
    case VAL_MAP: {
        ValueMap* m = (ValueMap*)obj_to_ptr(obj);
        /* 有效性校验：防止已释放/损坏对象的 buckets 指针导致崩溃 */
        if (m->buckets && (!GC_VALID_PTR(m->buckets) || ((unsigned long long)m->buckets & 0xF) != 0))
            break;
        if (m->cap <= 0) break;
        /* buckets/tree 内部缓冲区标记黑色（防止新生代缓冲区被 Minor GC sweep） */
        if (m->buckets) gc_mark_internal_black(m->buckets);
        if (m->tree) gc_mark_internal_black(m->tree);
        for (int i = 0; i < m->cap; i++) {
            MapEntry* e = m->buckets[i];
            if (!e) continue;
            if (m->tree[i]) {
                MapEntry* stk[256];
                int top = 0;
                MapEntry* cur = e;
                while (cur || top > 0) {
                    while (cur) {
                        if (top < 256) stk[top++] = cur;
                        cur = cur->left;
                    }
                    if (top == 0) break;
                    cur = stk[--top];
                    gc_mark_internal_black(cur);  /* MapEntry 老年代，标记黑色 */
                    gc_mark_value_to_stack_minor(cur->key);
                    gc_mark_value_to_stack_minor(cur->value);
                    cur = cur->right;
                }
            } else {
                while (e) {
                    gc_mark_internal_black(e);
                    gc_mark_value_to_stack_minor(e->key);
                    gc_mark_value_to_stack_minor(e->value);
                    e = e->next;
                }
            }
        }
        break;
    }
    default:
        break;
    }
    obj->marked = 1;  /* 黑色 */
}

/* 扫描 remembered set 中的所有老年代对象，将它们引用的新生代子对象推入标记栈。
 * 老年代对象本身不标记（Minor GC 不 sweep 老年代），但其内部缓冲区标记黑色。 */
static void gc_scan_remembered_set(void)
{
    for (size_t i = 0; i < g_rs_size; i++) {
        GCObject* obj = g_remembered_set[i];
        if (!obj) continue;
        /* 老年代对象：扫描其子对象，将新生代子对象推入标记栈。
         * 复用 gc_mark_one_minor 逻辑（它会标记内部缓冲区黑色）。
         * 注意：gc_mark_one_minor 会设 obj->marked=1，但 Minor GC 结束时
         * 会清除所有非永生对象的 marked 位，所以无副作用。 */
        gc_mark_one_minor(obj);
    }
}

/* ============================================================
 * 根扫描（推入标记栈而非递归标记）
 * ============================================================ */

/* 扫描单个 VM 栈 + 帧链 */
static void gc_scan_vm_roots_to_stack(Value* stack, int sp, StackFrame* frame)
{
    if (stack) {
        for (int i = 0; i < sp; i++) {
            gc_mark_value_to_stack(stack[i]);
        }
    }
    StackFrame* f = frame;
    while (f) {
        if (f->vals) {
            for (int i = 0; i < f->cnt; i++) {
                gc_mark_value_to_stack(f->vals[i]);
            }
        }
        f = f->parent;
    }
}

/* 扫描单个 CFrame 链（操作数栈 + 局部变量指针数组） */
static void gc_scan_cframe_chain_to_stack(CFrame* cf)
{
    while (cf) {
        if (cf->stack && cf->sp) {
            for (int i = 0; i < *cf->sp; i++) {
                gc_mark_value_to_stack(cf->stack[i]);
            }
        }
        if (cf->local_ptrs) {
            for (int i = 0; i < cf->nlocals; i++) {
                if (cf->local_ptrs[i]) {
                    gc_mark_value_to_stack(*cf->local_ptrs[i]);
                }
            }
        }
        cf = cf->parent;
    }
}

/* 扫描所有线程根：
 * - g_gc_threads 注册表中所有线程的 stack[0..*sp_ptr] + frame 链局部变量
 * - g_gc_cframe_threads 注册表中所有线程的 CFrame 链
 * - 安全兜底：注册表为空时使用传入的 stack/sp/frame 参数 */
/* ---- 全局根回调：外部模块（如线程表）注册需扫描的全局堆引用 ---- */
typedef void (*GCGlobalRootScanFn)(void);
static GCGlobalRootScanFn g_global_root_scan = NULL;

void gc_register_global_root_scan(GCGlobalRootScanFn fn) { g_global_root_scan = fn; }

/* ---- 保守式 C 栈扫描的 GC 指针集合 ----
 * 扫描前遍历 g_gc_objects，把所有活跃对象的用户数据指针存入开放寻址哈希集合。
 * 保守扫描时只标记集合中确认存在的指针，避免：
 *   (a) 把栈上非 GC 指针（编译器阶段 strdup 符号、代码指针等）误判为 GC 对象；
 *   (b) 回退 GCObject 头时读到 ASan 红区 / 未映射内存 → segfault。
 * 这是 ASan 下保守扫描的安全前提：绝不解引用未经验证的指针。 */
static void** g_cstack_ptr_set = NULL;   /* 开放寻址哈希集合（存用户数据指针） */
static size_t  g_cstack_set_cap = 0;     /* 容量（2 的幂） */
static size_t  g_cstack_set_used = 0;    /* 已用槽数 */

/* 初始化/重建哈希集合（GC 标记阶段开始前调用，已持 g_gc_mutex）。
 * 遍历 g_gc_objects，插入所有活跃对象的用户数据指针。
 * 容量按当前对象数 2x 扩容（load factor < 0.5），动态增长。 */
static void gc_cstack_set_build(void)
{
    /* 统计对象数，确定容量 */
    size_t cnt = 0;
    for (GCObject* o = g_gc_objects; o; o = o->next) cnt++;
    size_t need = cnt * 2 + 16;
    /* 取 >= need 的最小 2 的幂 */
    size_t cap = 1;
    while (cap < need) cap <<= 1;
    if (cap != g_cstack_set_cap) {
        free(g_cstack_ptr_set);
        g_cstack_ptr_set = (void**)calloc(cap, sizeof(void*));
        g_cstack_set_cap = g_cstack_ptr_set ? cap : 0;
    } else if (g_cstack_ptr_set) {
        memset(g_cstack_ptr_set, 0, g_cstack_set_cap * sizeof(void*));
    }
    g_cstack_set_used = 0;
    if (!g_cstack_ptr_set) return;
    size_t mask = g_cstack_set_cap - 1;
    for (GCObject* o = g_gc_objects; o; o = o->next) {
        if (o->flags & 0x02) continue;  /* 跳过内部缓冲区（items/buckets） */
        void* up = obj_to_ptr(o);
        size_t h = ((size_t)up >> 4) & mask;  /* 16 字节对齐后哈希 */
        while (g_cstack_ptr_set[h] != NULL) h = (h + 1) & mask;
        g_cstack_ptr_set[h] = up;
        g_cstack_set_used++;
    }
}

/* 查询指针是否在 GC 对象集合中。 */
static int gc_cstack_set_contains(void* ptr)
{
    if (!g_cstack_ptr_set || !ptr) return 0;
    size_t mask = g_cstack_set_cap - 1;
    size_t h = ((size_t)ptr >> 4) & mask;
    while (g_cstack_ptr_set[h] != NULL) {
        if (g_cstack_ptr_set[h] == ptr) return 1;
        h = (h + 1) & mask;
    }
    return 0;
}

/* 检查地址是否落在某个 CFrame 的 __stk 操作数栈区域内。
 * __stk 已由 CFrame 精确扫描覆盖 [0, *sp)，保守式扫描需跳过整个 __stk 数组
 * （包括 *sp 之后的陈旧槽位），避免循环中旧对象指针被误标记为根。 */
static int gc_addr_in_cframe_stack(CFrame* cf, uintptr_t addr)
{
    while (cf) {
        if (cf->stack && cf->stack_size > 0) {
            uintptr_t start = (uintptr_t)cf->stack;
            uintptr_t end = start + (uintptr_t)cf->stack_size * sizeof(Value);
            if (addr >= start && addr < end) return 1;
        }
        cf = cf->parent;
    }
    return 0;
}

/* 保守式 C 栈扫描：扫描线程栈上所有可能是 GC 用户数据指针的 word。
 * Boehm GC 经典做法，兜底捕获 precise root scanning 遗漏的 C 局部变量。
 *
 * 关键优化：跳过所有 CFrame 的 __stk 操作数栈区域——该区域已由 CFrame 精确扫描
 * 覆盖 [0, *sp)，而 *sp 之后的陈旧槽位包含循环中旧对象指针，会导致严重误标记。
 *
 * 安全设计：
 *   1. 扫描前已通过 gc_cstack_set_build() 构建了所有 GC 对象用户指针的哈希集合，
 *      只标记集合中确认存在的指针，绝不回退 GCObject 头读任意内存。
 *   2. 扫描下界用 safepoint 时的实际栈指针（c_stack_sp），而非固定 2MB 窗口——
 *      固定窗口会读到栈底以下的未映射内存（ASan stack-buffer-overflow）。
 *      栈向下增长：top 是注册时栈顶（高地址），bottom 是 safepoint 栈指针（低地址）。 */
static void gc_conservative_cstack_scan(void* top, void* bottom, CFrame* cf_chain)
{
    if (!top) return;
    uintptr_t* p = (uintptr_t*)top;
    /* 下界：取 safepoint 栈指针；若未记录则退化为 top-2MB（兜底） */
    uintptr_t* end = bottom ? (uintptr_t*)bottom
                            : (uintptr_t*)((char*)top - 2 * 1024 * 1024);
    /* 安全：下界不能高于上界（异常情况） */
    if (end > p) end = p;
    for (; p >= end; p--) {
        /* 跳过 __stk 操作数栈区域（已由 CFrame 精确扫描，陈旧槽位会误标记） */
        if (gc_addr_in_cframe_stack(cf_chain, (uintptr_t)p)) continue;
#ifdef GC_ASAN_ENABLED
        /* ASan 在栈帧之间插入红区（f1/f2/f3）。读 *p 前检查是否 poisoned，
         * poisoned 则跳过该 word（不读，避免 stack-buffer-overflow）。 */
        if (__asan_region_is_poisoned(p, sizeof(uintptr_t))) continue;
#endif
        uintptr_t word = *p;
        if (!GC_VALID_PTR((void*)word)) continue;
        /* 只标记哈希集合中确认存在的 GC 对象指针 */
        if (!gc_cstack_set_contains((void*)word)) continue;
        /* C 栈是根：栈上直接引用的所有对象都必须标记，不分年代。
         * 之前 Minor GC 跳过老年代对象（假设由 remembered set 追踪），但老年代对象
         * 直接在 C 栈上（如 val_array 的 items 局部指针，其父 ValueArray 是新生代
         * 不在 remembered set 中）时会漏标 → marked==0 → Minor GC sweep 误释放 → UAF。 */
        gc_mark_ptr_to_stack((void*)word);
    }
}

void gc_scan_roots_to_stack(Value* stack, int sp, StackFrame* frame)
{
    if (g_gc_threads) {
        for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
            gc_scan_vm_roots_to_stack(e->stack, e->sp_ptr ? *e->sp_ptr : 0, e->frame);
            gc_conservative_cstack_scan(e->c_stack_top, e->c_stack_sp, NULL);
        }
    } else {
        gc_scan_vm_roots_to_stack(stack, sp, frame);
    }

    /* 扫描编译通道 CFrame 链 */
    for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
        CFrame* cf = *e->cframe_ptr;
        gc_scan_cframe_chain_to_stack(cf);
        gc_conservative_cstack_scan(e->c_stack_top, e->c_stack_sp, cf);
    }
    /* 安全兜底：扫描当前线程 tls_cframe（未注册的情况） */
    if (tls_cframe && !g_gc_cframe_threads) {
        gc_scan_cframe_chain_to_stack(tls_cframe);
    }
    /* 扫描外部模块注册的全局根（如线程表中的待 join 结果） */
    if (g_global_root_scan) g_global_root_scan();
}

/* Minor GC 专用：扫描单个 VM 栈 + 帧链（只标记新生代） */
static void gc_scan_vm_roots_minor(Value* stack, int sp, StackFrame* frame)
{
    if (stack) {
        for (int i = 0; i < sp; i++) {
            gc_mark_value_to_stack_minor(stack[i]);
        }
    }
    StackFrame* f = frame;
    while (f) {
        if (f->vals) {
            for (int i = 0; i < f->cnt; i++) {
                gc_mark_value_to_stack_minor(f->vals[i]);
            }
        }
        f = f->parent;
    }
}

/* Minor GC 专用：扫描单个 CFrame 链（只标记新生代） */
static void gc_scan_cframe_chain_minor(CFrame* cf)
{
    while (cf) {
        if (cf->stack && cf->sp) {
            for (int i = 0; i < *cf->sp; i++) {
                gc_mark_value_to_stack_minor(cf->stack[i]);
            }
        }
        if (cf->local_ptrs) {
            for (int i = 0; i < cf->nlocals; i++) {
                if (cf->local_ptrs[i]) {
                    gc_mark_value_to_stack_minor(*cf->local_ptrs[i]);
                }
            }
        }
        cf = cf->parent;
    }
}

/* Minor GC 专用：扫描所有线程根（VM 栈 + 帧链 + CFrame 链），只标记新生代 */
static void gc_scan_roots_minor(Value* stack, int sp, StackFrame* frame)
{
    if (g_gc_threads) {
        for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
            gc_scan_vm_roots_minor(e->stack, e->sp_ptr ? *e->sp_ptr : 0, e->frame);
            gc_conservative_cstack_scan(e->c_stack_top, e->c_stack_sp, NULL);
        }
    } else {
        gc_scan_vm_roots_minor(stack, sp, frame);
    }
    for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
        CFrame* cf = *e->cframe_ptr;
        gc_scan_cframe_chain_minor(cf);
        gc_conservative_cstack_scan(e->c_stack_top, e->c_stack_sp, cf);
    }
    if (tls_cframe && !g_gc_cframe_threads) {
        gc_scan_cframe_chain_minor(tls_cframe);
    }
    /* 扫描外部模块注册的全局根（如线程表中的待 join 结果） */
    if (g_global_root_scan) g_global_root_scan();
}

/* ============================================================
 * 写屏障实现（Dijkstra 风格）
 *
 * 并发标记期间，若新值引用白色堆对象，将其变灰入标记栈，
 * 防止黑色对象引用白色对象导致三色不变式破坏。
 * 逻辑与 gc_mark_value_to_stack 相同，但不需要 g_gc_mutex
 * （标记栈有自己的 mutex）。
 * ============================================================ */
void gc_write_barrier_impl(Value v)
{
    gc_mark_value_to_stack(v);
}

/* ============================================================
 * 清除：遍历全局链表，未标记的释放/回收，清除标记位
 *
 * 小对象（user_size <= TLA_MAX_SIZE）：不 free，从 g_gc_objects 摘除
 *   后移入全局空闲链表（marked=2，用 next 链接，不触碰用户数据区域）
 * 大对象：free()，从链表摘除
 * 永生对象（marked==2，含钉住对象和本地空闲链表对象）：跳过
 * ============================================================ */
void gc_sweep(void)
{
    GCObject** pp = &g_gc_objects;
    while (*pp) {
        GCObject* cur = *pp;
        if (cur->marked == 2) {
            /* 永生对象（钉住 / 本地空闲链表预分配）：跳过 */
            pp = &cur->next;
        } else if (!cur->marked) {
            /* 未标记：需要回收 */
            if (cur->user_size <= TLA_MAX_SIZE) {
                *pp = cur->next;
                g_gc_bytes -= sizeof(GCObject) + cur->user_size;
                free(cur);
            } else {
                /* 大对象：free */
                *pp = cur->next;
                g_gc_bytes -= sizeof(GCObject) + cur->user_size;
                free(cur);
            }
        } else {
            /* 已标记（活跃）：清除标记位，继续 */
            cur->marked = 0;
            pp = &cur->next;
        }
    }
}

/* ============================================================
 * Minor GC 专用 sweep：只回收新生代对象，存活对象 age++，达到阈值晋升老年代
 *
 * 与 gc_sweep 的区别：
 *   - 新生代垃圾（age < PROMOTE_AGE 且 marked==0）：回收（同 gc_sweep）
 *   - 新生代存活（age < PROMOTE_AGE 且 marked!=0）：age++，若 age>=PROMOTE_AGE 则晋升
 *   - 老年代对象（age >= PROMOTE_AGE）：不回收，仅清除标记位（标记位不能残留到下一轮）
 *   - 永生对象（marked==2）：跳过
 * ============================================================ */
static void gc_sweep_minor(void)
{
    GCObject** pp = &g_gc_objects;
    while (*pp) {
        GCObject* cur = *pp;
        if (cur->marked == 2) {
            /* 永生对象（钉住 / 本地空闲链表预分配）：跳过 */
            pp = &cur->next;
        } else if (cur->age < PROMOTE_AGE) {
            /* ---- 新生代对象 ----
             * 存活判定：只有 marked==1（标记阶段通过 gc_mark_one_minor 实际置黑）
             * 才是真正存活。marked==0（白色）与 marked==3（新对象预标记，但本轮
             * 标记阶段未触达）均视为垃圾回收。
             *
             * 为什么 marked==3 必须回收：marked=3 是为 Major GC 增量标记设计的
             * "新对象预标记"。但 Minor GC 是 STW 全量标记，标记阶段会把所有从根
             * 可达的对象经 gc_mark_ptr_to_stack 推入灰色栈并最终置为 marked=1。
             * 因此 Minor GC 后仍为 marked=3 的对象必然未被根触达，是垃圾。
             * 旧代码用 `!cur->marked` 判定，导致 marked=3 走 else 分支被误判存活、
             * age++ 且不回收，新生代垃圾全部滞留，年轻字节数不降 → 性能崩溃。 */
            if (cur->marked != 1) {
                /* 新生代垃圾：回收 */
                size_t obj_bytes = sizeof(GCObject) + cur->user_size;
                if (cur->user_size <= TLA_MAX_SIZE) {
                    *pp = cur->next;
                    g_gc_bytes -= obj_bytes;
                    g_young_bytes -= obj_bytes;
                    free(cur);
                } else {
                    /* 大对象：free（理论上大对象直接老年代，不会出现在新生代，但防御性处理） */
                    *pp = cur->next;
                    g_gc_bytes -= obj_bytes;
                    g_young_bytes -= obj_bytes;
                    free(cur);
                }
            } else {
                /* 新生代存活：age++，可能晋升老年代 */
                cur->age++;
                if (cur->age >= PROMOTE_AGE) {
                    /* 晋升：从新生代统计移到老年代统计 */
                    size_t obj_bytes = sizeof(GCObject) + cur->user_size;
                    g_young_bytes -= obj_bytes;
                    g_old_bytes += obj_bytes;
                    /* 晋升对象加入 remembered set（保守策略：刚从新生代来，
                     * 很可能引用其他新生代对象，必须追踪以防下轮 Minor GC 丢失引用） */
                    remembered_set_add(cur);
                }
                cur->marked = 0;
                pp = &cur->next;
            }
        } else {
            /* ---- 老年代对象 ----
             * Minor GC 不回收老年代对象，仅清除标记位。
             * 老年代对象的回收由 Major GC 负责。之前在此处回收 marked==0 的老年代
             * 对象导致 UAF：remembered set 在 Major GC 后被清空，老年代对象不被根扫描
             * 覆盖（尤其 g_slots 中的悬挂引用、C 栈上的 items 指针等），marked 残留 0
             * 被误释放。老年代对象量大但 Minor GC 遍历 O(n) 开销可接受。 */
            cur->marked = 0;
            pp = &cur->next;
        }
    }
}

/* ============================================================
 * STW 轮询辅助 + 计时
 * ============================================================ */

#define STW_POLL_MAX_ITERS 100000000

/* 轮询所有注册线程到达安全点（带超时保护）。
 * 调用前必须已设置 g_gc_stw=1 和 GC 线程自身 at_safepoint=1。
 * 用 sched_yield() 自旋（macOS 定时器粒度约 1ms，nanosleep 开销大）。
 *
 * 每次遍历列表都持 g_gc_mutex：防止其他线程并发 register/unregister 修改链表时
 * 本线程读到已释放 entry 的 next 指针（use-after-free）。持锁期间 worker 线程
 * 在 gc_stw_check 自旋不需要 g_gc_mutex，无死锁。 */
static void gc_wait_all_threads_at_safepoint(void)
{
    int poll_iters = 0;
    while (1) {
        int all_paused = 1;
        pthread_mutex_lock(&g_gc_mutex);
        for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
            if (!e->at_safepoint) { all_paused = 0; break; }
        }
        if (all_paused) {
            for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
                if (!e->at_safepoint) { all_paused = 0; break; }
            }
        }
        pthread_mutex_unlock(&g_gc_mutex);
        if (all_paused) break;
        if (poll_iters >= STW_POLL_MAX_ITERS) {
            LOG_ERROR("GC: WARNING: STW timeout, some threads not at safepoint (blocked in IO/lock?)\n");
            break;
        }
        sched_yield();
        poll_iters++;
    }
}

/* 跨平台高精度计时：返回纳秒 */
static unsigned long long gc_now_ns(void)
{
#ifdef _WIN32
    static LARGE_INTEGER s_freq = {0};
    LARGE_INTEGER now;
    if (s_freq.QuadPart == 0) QueryPerformanceFrequency(&s_freq);
    QueryPerformanceCounter(&now);
    return (unsigned long long)(now.QuadPart * 1000000000ULL / s_freq.QuadPart);
#elif defined(__APPLE__)
    static mach_timebase_info_data_t s_tb = {0};
    if (s_tb.denom == 0) mach_timebase_info(&s_tb);
    return (unsigned long long)mach_absolute_time() * s_tb.numer / s_tb.denom;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
#endif
}

/* 读取增量标记开关（首次调用时读取环境变量） */
static int gc_incremental_enabled(void)
{
    if (!g_gc_incremental_checked) {
        g_gc_incremental_checked = 1;
        const char* env = getenv("LUMYR_GC_INCREMENTAL");
        if (env && strcmp(env, "0") == 0) {
            g_gc_incremental = 0;
        }
    }
    return g_gc_incremental;
}

/* 读取分代 GC 开关（首次调用时读取环境变量） */
static int gc_generational_enabled(void)
{
    if (!g_gc_generational_checked) {
        g_gc_generational_checked = 1;
        const char* env = getenv("LUMYR_GC_GENERATIONAL");
        if (env && strcmp(env, "0") == 0) {
            g_gc_generational = 0;
        }
    }
    return g_gc_generational;
}

/* 精确重新计算 g_gc_bytes / g_young_bytes / g_old_bytes（GC sweep 后调用，消除计数器漂移） */
static void gc_recount_bytes(void)
{
    g_gc_bytes = 0;
    g_young_bytes = 0;
    g_old_bytes = 0;
    for (GCObject* o = g_gc_objects; o; o = o->next) {
        size_t b = sizeof(GCObject) + o->user_size;
        g_gc_bytes += b;
        if (o->age < PROMOTE_AGE) g_young_bytes += b;
        else g_old_bytes += b;
    }
}

/* 判断是否需要触发 GC 及类型。
 * 返回 1=需要 Minor GC，2=需要 Major GC，0=不需要。
 * 调用方应在持有 g_gc_mutex 时调用（读取阈值和字节数）。 */
static int gc_need_collect_locked(void)
{
    if (atomic_load_explicit(&g_in_gc, memory_order_relaxed) || g_gc_disable != 0) return 0;
    if (gc_generational_enabled()) {
        /* 老年代阈值优先：老年代满了必须 Major GC 才能回收 */
        if (g_old_bytes > g_old_threshold) return 2;
        if (g_young_bytes > g_young_threshold) return 1;
        return 0;
    } else {
        return (g_gc_bytes > g_gc_threshold) ? 2 : 0;
    }
}

/* 暴露累计 STW 停顿时间（纳秒） */
unsigned long long gc_stw_time_ns(void) { return g_stw_total_ns; }

/* ============================================================
 * Major GC：全量增量标记-清除（sweep 全部对象，age 不变）
 *   - 增量模式：初始STW + 并发标记 + 最终STW + sweep
 *   - fallback：全量 STW 标记-清除
 *   - sweep 后所有存活对象 age 不变（老年代不降级）
 *   - 更新老年代阈值（和新生代阈值，因为 Major 也 sweep 了新生代）
 * ============================================================ */
void gc_collect_major(Value* stack, int sp, StackFrame* frame)
{
    /* 原子 CAS：确保只有一个线程进入 GC，防止多线程并发标记/回收竞态 */
    if (atomic_exchange_explicit(&g_in_gc, 1, memory_order_acquire)) return;
    g_major_gc_count++;

    if (!gc_incremental_enabled()) {
        /* === Fallback：全量 STW 标记（原有逻辑） === */
        unsigned long long t_fb_start = gc_now_ns();
        g_gc_stw = 1;
        gc_set_self_at_safepoint(1);
        gc_wait_all_threads_at_safepoint();

        pthread_mutex_lock(&g_gc_mutex);

        /* 递归标记所有根 */
        if (g_gc_threads) {
            for (GCThreadEntry* e = g_gc_threads; e; e = e->next) {
                gc_mark_roots(e->stack, e->sp_ptr ? *e->sp_ptr : 0, e->frame);
            }
        } else {
            gc_mark_roots(stack, sp, frame);
        }
        for (GCCFrameEntry* e = g_gc_cframe_threads; e; e = e->next) {
            CFrame* cf = *e->cframe_ptr;
            while (cf) {
                if (cf->stack && cf->sp) {
                    for (int i = 0; i < *cf->sp; i++) gc_mark(cf->stack[i]);
                }
                if (cf->local_ptrs) {
                    for (int i = 0; i < cf->nlocals; i++) {
                        if (cf->local_ptrs[i]) gc_mark(*cf->local_ptrs[i]);
                    }
                }
                cf = cf->parent;
            }
        }
        if (tls_cframe && !g_gc_cframe_threads) {
            CFrame* cf = tls_cframe;
            while (cf) {
                if (cf->stack && cf->sp) {
                    for (int i = 0; i < *cf->sp; i++) gc_mark(cf->stack[i]);
                }
                if (cf->local_ptrs) {
                    for (int i = 0; i < cf->nlocals; i++) {
                        if (cf->local_ptrs[i]) gc_mark(*cf->local_ptrs[i]);
                    }
                }
                cf = cf->parent;
            }
        }

        /* Major GC：sweep 前清空 remembered set（避免 sweep 后悬空指针） */
        remembered_set_clear();
        gc_sweep();
        gc_recount_bytes();
        /* 更新阈值：非分代模式用总字节，分代模式用老年代字节 */
        size_t new_threshold = g_gc_bytes * 2;
        if (new_threshold < 1024 * 1024) new_threshold = 1024 * 1024;
        g_gc_threshold = new_threshold;
        size_t new_old_thr = g_old_bytes * 2;
        if (new_old_thr < 1024 * 1024) new_old_thr = 1024 * 1024;
        g_old_threshold = new_old_thr;
        size_t new_young_thr = g_young_bytes * 2;
        if (new_young_thr < 1024 * 1024) new_young_thr = 1024 * 1024;
        g_young_threshold = new_young_thr;
        pthread_mutex_unlock(&g_gc_mutex);

        g_gc_stw = 0;
        gc_set_self_at_safepoint(0);
        unsigned long long t_fb_stw = gc_now_ns() - t_fb_start;
        g_stw_total_ns += t_fb_stw;

        /* LUMYR_GC_STATS=1 时输出统计（与增量路径格式对齐：initial=final=总停顿） */
        const char* stats_env = getenv("LUMYR_GC_STATS");
        if (stats_env && strcmp(stats_env, "1") == 0) {
            LOG_ERROR("[GC major] initial STW=%lluus final STW=0us total STW=%llums minor=%llu major=%llu young=%zuKB old=%zuKB\n",
                    t_fb_stw / 1000, g_stw_total_ns / 1000000,
                    g_minor_gc_count, g_major_gc_count, g_young_bytes / 1024, g_old_bytes / 1024);
        }
        atomic_store_explicit(&g_in_gc, 0, memory_order_release);
        return;
    }

    /* === 增量标记模式 === */

    /* ---- 初始 STW ---- */
    unsigned long long t_stw_start = gc_now_ns();
    g_gc_stw = 1;
    gc_set_self_at_safepoint(1);
    gc_wait_all_threads_at_safepoint();

    pthread_mutex_lock(&g_gc_mutex);

    /* 开启写屏障（必须在根扫描前开启） */
    g_gc_marking = 1;

    /* GC 发起者更新自身 c_stack_sp，使保守 C 栈扫描覆盖当前帧 */
    gc_record_self_cstack_sp();

    /* 构建保守式 C 栈扫描的 GC 指针集合（已持锁，遍历 g_gc_objects 安全）。
     * 初始 STW 构建一次即可；最终 STW 重扫时复用（新分配对象已 marked=1/3，
     * 不在集合中也不影响正确性——它们本就不会被 sweep）。 */
    gc_cstack_set_build();

    /* 扫描所有根，白色对象变灰入栈 */
    gc_scan_roots_to_stack(stack, sp, frame);

    pthread_mutex_unlock(&g_gc_mutex);

    /* 恢复应用线程（结束初始 STW） */
    g_gc_stw = 0;
    gc_set_self_at_safepoint(0);
    unsigned long long t_initial_stw = gc_now_ns() - t_stw_start;

    /* ---- 并发标记 ---- */
    while (1) {
        pthread_mutex_lock(&g_mark_stack_mutex);
        GCObject* obj = mark_stack_pop_locked();
        pthread_mutex_unlock(&g_mark_stack_mutex);
        if (!obj) break;
        gc_mark_one(obj);
    }

    /* ---- 最终 STW ---- */
    unsigned long long t_final_start = gc_now_ns();
    g_gc_stw = 1;
    gc_set_self_at_safepoint(1);
    gc_wait_all_threads_at_safepoint();

    pthread_mutex_lock(&g_gc_mutex);

    /* 重新扫描根 */
    gc_scan_roots_to_stack(stack, sp, frame);

    /* 排空标记栈 */
    while (1) {
        pthread_mutex_lock(&g_mark_stack_mutex);
        GCObject* obj = mark_stack_pop_locked();
        pthread_mutex_unlock(&g_mark_stack_mutex);
        if (!obj) break;
        gc_mark_one(obj);
    }

    /* 关闭写屏障 */
    g_gc_marking = 0;

    /* Major GC：sweep 前清空 remembered set（避免 sweep 后悬空指针） */
    remembered_set_clear();
    /* Major GC：sweep 全部对象 */
    gc_sweep();

    /* 精确重新计算字节数 + 更新阈值 */
    gc_recount_bytes();
    size_t new_threshold = g_gc_bytes * 2;
    if (new_threshold < 1024 * 1024) new_threshold = 1024 * 1024;
    g_gc_threshold = new_threshold;
    size_t new_old_thr = g_old_bytes * 2;
    if (new_old_thr < 1024 * 1024) new_old_thr = 1024 * 1024;
    g_old_threshold = new_old_thr;
    size_t new_young_thr = g_young_bytes * 2;
    if (new_young_thr < 1024 * 1024) new_young_thr = 1024 * 1024;
    g_young_threshold = new_young_thr;

    pthread_mutex_unlock(&g_gc_mutex);

    g_gc_stw = 0;
    gc_set_self_at_safepoint(0);
    unsigned long long t_final_stw = gc_now_ns() - t_final_start;

    /* 累计 STW 停顿 */
    g_stw_total_ns += t_initial_stw + t_final_stw;

    /* LUMYR_GC_STATS=1 时输出统计 */
    const char* stats_env = getenv("LUMYR_GC_STATS");
    if (stats_env && strcmp(stats_env, "1") == 0) {
        LOG_ERROR("[GC major] initial STW=%lluus final STW=%lluus total STW=%llums minor=%llu major=%llu young=%zuKB old=%zuKB\n",
                t_initial_stw / 1000, t_final_stw / 1000, g_stw_total_ns / 1000000,
                g_minor_gc_count, g_major_gc_count, g_young_bytes / 1024, g_old_bytes / 1024);
    }

    atomic_store_explicit(&g_in_gc, 0, memory_order_release);
}

/* ============================================================
 * Minor GC：真正的分代 GC —— 全量 STW 标记新生代 + remembered set，只 sweep 新生代
 *
 * 标记流程：
 *   1. STW 暂停所有线程
 *   2. 扫描所有线程根，只将新生代对象推入标记栈
 *   3. 扫描 remembered set 中的老年代对象，将它们引用的新生代子对象推入栈
 *   4. 迭代处理标记栈（gc_mark_one_minor），只标记新生代子对象
 *   5. sweep 新生代白色对象，存活对象 age++，达到阈值晋升老年代
 *   6. 晋升对象加入 remembered set（保守策略：刚从新生代来，很可能引用新生代）
 *
 * 不使用增量标记：标记范围小（只新生代），STW 时间短，不需要增量复杂度。
 * g_gc_marking 保持 0，写屏障空操作（STW 期间无并发修改）。
 * ============================================================ */
void gc_collect_minor(Value* stack, int sp, StackFrame* frame)
{
    /* 原子 CAS：确保只有一个线程进入 GC，防止多线程并发标记/回收竞态 */
    if (atomic_exchange_explicit(&g_in_gc, 1, memory_order_acquire)) return;
    g_minor_gc_count++;

    unsigned long long t_stw_start = gc_now_ns();

    /* ---- STW：暂停所有线程 ---- */
    g_gc_stw = 1;
    gc_set_self_at_safepoint(1);
    gc_wait_all_threads_at_safepoint();

    pthread_mutex_lock(&g_gc_mutex);

    /* 不开启增量写屏障（g_gc_marking 保持 0）：STW 期间无并发修改 */

    /* 重置标记栈（确保无残留） */
    g_mark_stack_size = 0;

    /* GC 发起者更新自身 c_stack_sp，使保守 C 栈扫描覆盖当前帧 */
    gc_record_self_cstack_sp();

    /* 构建保守式 C 栈扫描的 GC 指针集合（已持锁，遍历 g_gc_objects 安全） */
    gc_cstack_set_build();

    /* ---- 1. 扫描所有线程根，只标记新生代 ---- */
    gc_scan_roots_minor(stack, sp, frame);

    /* ---- 2. 扫描 remembered set：老年代对象引用的新生代 ---- */
    gc_scan_remembered_set();

    /* ---- 3. 迭代处理标记栈 ---- */
    size_t marked_count = 0;
    while (1) {
        GCObject* obj = mark_stack_pop_locked();
        if (!obj) break;
        gc_mark_one_minor(obj);
        marked_count++;
    }

    /* ---- 4. sweep 新生代（存活 age++ + 晋升 + 晋升对象入 rs） ---- */
    gc_sweep_minor();

    /* ---- 5. 重新计算字节数 + 更新新生代阈值 ---- */
    gc_recount_bytes();
    size_t new_young_thr = g_young_bytes * 2;
    if (new_young_thr < 1024 * 1024) new_young_thr = 1024 * 1024;
    g_young_threshold = new_young_thr;

    pthread_mutex_unlock(&g_gc_mutex);

    /* ---- 结束 STW ---- */
    g_gc_stw = 0;
    gc_set_self_at_safepoint(0);

    unsigned long long t_stw = gc_now_ns() - t_stw_start;
    g_stw_total_ns += t_stw;

    /* LUMYR_GC_STATS=1 时输出统计 */
    const char* stats_env = getenv("LUMYR_GC_STATS");
    if (stats_env && strcmp(stats_env, "1") == 0) {
        LOG_ERROR("[GC minor] STW=%lluus total STW=%llums marked=%zu rs=%zu minor=%llu major=%llu young=%zuKB old=%zuKB\n",
                t_stw / 1000, g_stw_total_ns / 1000000, marked_count, g_rs_size,
                g_minor_gc_count, g_major_gc_count, g_young_bytes / 1024, g_old_bytes / 1024);
    }

    atomic_store_explicit(&g_in_gc, 0, memory_order_release);
}

/* ============================================================
 * GC 调度入口
 *   - 分代 GC 启用时：触发 Minor GC（新生代优先回收，开销小）
 *   - 分代 GC 禁用时：直接调用 Major GC（全量标记-清除，与原有行为一致）
 *   - 显式 gc_collect_now() 应直接调用 gc_collect_major()（全量回收）
 * ============================================================ */
void gc_collect(Value* stack, int sp, StackFrame* frame)
{
    if (gc_generational_enabled()) {
        gc_collect_minor(stack, sp, frame);
    } else {
        gc_collect_major(stack, sp, frame);
    }
}

/* ============================================================
 * GC 暂停/恢复（构造复合对象时防止中间态被 sweep）
 * ============================================================ */
void gc_disable(void)
{
    pthread_mutex_lock(&g_gc_mutex);
    g_gc_disable++;
    pthread_mutex_unlock(&g_gc_mutex);
}

void gc_enable(void)
{
    pthread_mutex_lock(&g_gc_mutex);
    if (g_gc_disable > 0) g_gc_disable--;
    /* 计数器归零时若超过阈值则触发一次 GC（构造函数内批量分配常伴随 gc_disable，
     * 自动 GC 条件在 gc_alloc 中被 g_gc_disable 阻塞，需在 enable 时补触发） */
    int need_type = gc_need_collect_locked();
    pthread_mutex_unlock(&g_gc_mutex);
    if (need_type && tls_stack && tls_sp && (tls_frame || tls_cframe)) {
        if (need_type == 1) gc_collect_minor(tls_stack, *tls_sp, tls_frame);
        else gc_collect_major(tls_stack, *tls_sp, tls_frame);
    }
}

/* ============================================================
 * 根注册
 * ============================================================ */
void gc_set_roots(Value* stack, int* sp_ptr, StackFrame* frame)
{
    tls_stack = stack;
    tls_sp = sp_ptr;
    tls_frame = frame;
}

void gc_get_roots(Value** stack, int** sp_ptr, StackFrame** frame)
{
    if (stack) *stack = tls_stack;
    if (sp_ptr) *sp_ptr = tls_sp;
    if (frame) *frame = tls_frame;
}

/* 手动触发一次 GC（使用当前注册的根）。
 * 直接调用 gc_collect_major() 进行全量回收——手动 gc_collect() 语义上应回收所有
 * 不可达对象（包括老年代），而非仅 Minor GC。自动 GC（gc_alloc 触发）仍走
 * gc_collect() 调度入口（优先 Minor，阈值超限时 Major）。 */
void gc_collect_now(void)
{
    if (tls_stack && tls_sp && (tls_frame || tls_cframe)) {
        gc_collect_major(tls_stack, *tls_sp, tls_frame);
    }
}

/* ============================================================
 * 多线程栈根注册表
 * ============================================================ */
void gc_register_thread(Value* stack, int* sp_ptr, StackFrame* frame)
{
    GCThreadEntry* e = (GCThreadEntry*)malloc(sizeof(GCThreadEntry));
    if (!e) {
        LOG_ERROR("GC: out of memory registering thread\n");
        abort();
    }
    e->tid = pthread_self();
    e->stack = stack;
    e->sp_ptr = sp_ptr;
    e->frame = frame;
    e->at_safepoint = 0;
    e->c_stack_top = __builtin_frame_address(0);
    e->c_stack_sp = e->c_stack_top;

    pthread_mutex_lock(&g_gc_mutex);
    /* 头插：最近注册的 entry 在链表头部，unregister 时移除第一个匹配 tid 的 entry */
    e->next = g_gc_threads;
    g_gc_threads = e;
    pthread_mutex_unlock(&g_gc_mutex);

    /* 设置线程本地指针，供 gc_stw_check 设置 at_safepoint */
    tls_cur_vm_entry = e;
}

void gc_unregister_thread(void)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_gc_mutex);
    /* 移除第一个 tid == self 的 entry（头插保证它是最近注册的，栈式语义） */
    GCThreadEntry** pp = &g_gc_threads;
    GCThreadEntry* next_for_self = NULL;
    while (*pp) {
        if (pthread_equal((*pp)->tid, self)) {
            GCThreadEntry* victim = *pp;
            *pp = victim->next;
            /* 查找同线程的下一个 entry（嵌套注册时更新 TLS 指针） */
            for (GCThreadEntry* e = *pp; e; e = e->next) {
                if (pthread_equal(e->tid, self)) { next_for_self = e; break; }
            }
            free(victim);
            break;
        }
        pp = &(*pp)->next;
    }
    /* 清理线程本地空闲链表（tla_local_free）：
     * 其中的对象 marked=2（永生），线程退出后永远无法被重新分配或 GC 回收。
     * 将 marked 改为 0，使下一次 GC sweep 可回收它们。
     * 这些对象不被任何根引用（线程已退出），GC 标记阶段不会解析它们的 Value 数据。 */
    GCObject* tcur = tla_local_free;
    while (tcur) {
        if (tcur->marked == 2) tcur->marked = 0;
        tcur = TLA_LOCAL_NEXT(tcur);
    }
    tla_local_free = NULL;
    tla_local_count = 0;
    pthread_mutex_unlock(&g_gc_mutex);
    tls_cur_vm_entry = next_for_self;
}

/* 移除当前线程倒数第二个 entry（保留最近注册的 protect entry）。
 * 用于 OPC_RETURN 最外层：先 gc_protect_push(v) 注册 protect entry（head），
 * 再调用本函数移除 VM entry（第二个），实现 protect 与 unregister 无窗口衔接，
 * 避免 unregister→protect_push 之间 v 无根保护被误回收。 */
void gc_unregister_thread_keep_protect(void)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_gc_mutex);
    GCThreadEntry** pp = &g_gc_threads;
    /* 跳过第一个（protect entry） */
    while (*pp && !pthread_equal((*pp)->tid, self)) pp = &(*pp)->next;
    if (!*pp) { pthread_mutex_unlock(&g_gc_mutex); return; }
    pp = &(*pp)->next;
    /* 现在指向第二个 entry（VM entry） */
    while (*pp && !pthread_equal((*pp)->tid, self)) pp = &(*pp)->next;
    if (*pp) {
        GCThreadEntry* victim = *pp;
        *pp = victim->next;
        free(victim);
    }
    pthread_mutex_unlock(&g_gc_mutex);
}

/* ============================================================
 * 编译通道 CFrame 帧链管理
 * ============================================================ */
void gc_push_cframe(CFrame* f)
{
    /* 首次注册线程（内部加锁，必须在 push 加锁前完成，否则双重锁死锁） */
    if (!tls_cframe_registered) {
        gc_register_cframe_thread();
        tls_cframe_registered = 1;
    }
    pthread_mutex_lock(&g_gc_mutex);
    f->parent = tls_cframe;
    tls_cframe = f;
    pthread_mutex_unlock(&g_gc_mutex);
    /* tls_stack/tls_sp 是线程本地，仅当前线程访问，无竞态，锁外设置 */
    tls_stack = f->stack;
    tls_sp = f->sp;
}

void gc_pop_cframe(void)
{
    pthread_mutex_lock(&g_gc_mutex);
    CFrame* parent = NULL;
    if (tls_cframe) {
        parent = tls_cframe->parent;
        tls_cframe = parent;
    }
    pthread_mutex_unlock(&g_gc_mutex);
    if (parent) {
        tls_stack = parent->stack;
        tls_sp = parent->sp;
    } else {
        tls_stack = NULL;
        tls_sp = NULL;
    }
}

CFrame* gc_cframe_top(void)
{
    return tls_cframe;
}

void gc_cframe_restore(CFrame* top)
{
    pthread_mutex_lock(&g_gc_mutex);
    tls_cframe = top;
    pthread_mutex_unlock(&g_gc_mutex);
    if (top) {
        tls_stack = top->stack;
        tls_sp = top->sp;
    } else {
        tls_stack = NULL;
        tls_sp = NULL;
    }
}

void gc_register_cframe_thread(void)
{
    GCCFrameEntry* e = (GCCFrameEntry*)malloc(sizeof(GCCFrameEntry));
    if (!e) {
        LOG_ERROR("GC: out of memory registering cframe thread\n");
        abort();
    }
    e->tid = pthread_self();
    e->cframe_ptr = &tls_cframe;
    e->at_safepoint = 0;
    e->c_stack_top = __builtin_frame_address(0);
    e->c_stack_sp = e->c_stack_top;

    pthread_mutex_lock(&g_gc_mutex);
    e->next = g_gc_cframe_threads;
    g_gc_cframe_threads = e;
    pthread_mutex_unlock(&g_gc_mutex);

    /* 设置线程本地指针，供 gc_stw_check 设置 at_safepoint */
    tls_cur_cf_entry = e;
}

void gc_unregister_cframe_thread(void)
{
    pthread_t self = pthread_self();
    pthread_mutex_lock(&g_gc_mutex);
    GCCFrameEntry** pp = &g_gc_cframe_threads;
    GCCFrameEntry* next_for_self = NULL;
    while (*pp) {
        if (pthread_equal((*pp)->tid, self)) {
            GCCFrameEntry* victim = *pp;
            *pp = victim->next;
            for (GCCFrameEntry* e = *pp; e; e = e->next) {
                if (pthread_equal(e->tid, self)) { next_for_self = e; break; }
            }
            free(victim);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_gc_mutex);
    tls_cur_cf_entry = next_for_self;
    tls_cframe_registered = 0;
}

/* 统计 */
size_t gc_bytes(void) { return g_gc_bytes; }
size_t gc_young_bytes(void) { return g_young_bytes; }
size_t gc_old_bytes(void) { return g_old_bytes; }
size_t gc_count(void)
{
    size_t cnt = 0;
    pthread_mutex_lock(&g_gc_mutex);
    for (GCObject* o = g_gc_objects; o; o = o->next) cnt++;
    pthread_mutex_unlock(&g_gc_mutex);
    return cnt;
}
