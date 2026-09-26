// lm_thread.c —— 多线程运行时：线程表 + 通用线程启动/join
// 依赖上一批多线程预备：执行器状态 _Thread_local（vm.c / lumyr_value.c）、GC CAS 头插
#include "lm_thread.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

typedef struct {
    pthread_t handle;
    int used;       // 槽位占用
    int done;       // 线程已退出（结果已写）
    int id;         // 用户可见线程 id（自增）
    Value result;   // 线程返回值（join 时克隆取走）
} ThreadSlot;

/* 动态线程表：容量按需翻倍扩容，无硬上限（上限由系统资源/OS 决定）。
 * 注意 realloc 会移动表内存，但槽内只存值（handle/result），且所有访问都在
 * g_lock 锁内取下标的瞬时值，线程体只通过 job->slot 下标访问，扩容安全。 */
static ThreadSlot* g_slots = NULL;
static int g_cap = 0;                 // 当前容量
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 0;             // 线程 id 自增（int，实际到不了上限）

/* GC 全局根扫描：扫描所有已使用槽位中的 result 值。
 * 工作线程退出后，result 堆对象无根可达，必须由 GC 显式扫描防止误回收。
 * 注意：GC 在 STW 下运行，所有线程已暂停，无需取 g_lock（否则死锁：
 * 暂停线程可能正持有 g_lock）。Value 读取在 x86-64 对齐原子，安全。 */
static void gc_scan_thread_roots(void) {
    /* 必须持 g_lock：lumyr_thread_set_result/thread_join 在 g_lock 下读写
     * g_slots.result/used。不加锁读取会看到撕裂或旧值（如刚复用槽位但 result
     * 仍是上一轮的悬挂指针），导致 GC 标记阶段读已释放内存 → UAF。 */
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_cap; i++) {
        if (g_slots[i].used) {
            gc_mark_value_to_stack(g_slots[i].result);
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (getenv("GC_DEBUG_ROOTS")) {
        int n = 0;
        for (int i = 0; i < g_cap; i++) if (g_slots[i].used) n++;
        fprintf(stderr, "GC: scanned %d thread slots\n", n);
    }
}

typedef struct {
    ThreadBody body;
    void* data;
    int slot;
    int argc;
    Value* args;    // 克隆参数（线程退出后释放数组本身；值本体与全库一致不主动 free）
} ThreadJob;

static void* lm_thread_main(void* p)
{
    ThreadJob* job = (ThreadJob*)p;
    ThreadLaunch t;
    t.slot = job->slot;
    t.argc = job->argc;
    t.args = job->args;
    t.data = job->data;
    job->body(&t);   // 线程体：执行函数调用 + lumyr_thread_set_result
    pthread_mutex_lock(&g_lock);
    g_slots[job->slot].done = 1;
    pthread_mutex_unlock(&g_lock);
    free(job->args);
    free(job);
    return NULL;
}

int lumyr_thread_start(ThreadBody body, void* data, const Value* args, int argc)
{
    pthread_mutex_lock(&g_lock);
    if(!g_slots) {   // 首次：分配初始容量
        g_slots = (ThreadSlot*)calloc(LM_THREAD_INITIAL_CAP, sizeof(ThreadSlot));
        if(!g_slots) { pthread_mutex_unlock(&g_lock); runtime_error("thread: 内存不足"); }
        g_cap = LM_THREAD_INITIAL_CAP;
        /* 注册 GC 全局根扫描：确保线程表中的待 join 结果不被误回收 */
        gc_register_global_root_scan(gc_scan_thread_roots);
    }
    int slot = -1;
    for(int i = 0; i < g_cap; i++) {
        if(!g_slots[i].used) { slot = i; break; }
    }
    if(slot < 0) {
        // 容量翻倍扩容，新槽清零
        int newcap = g_cap * 2;
        ThreadSlot* ns = (ThreadSlot*)realloc(g_slots, (size_t)newcap * sizeof(ThreadSlot));
        if(!ns) { pthread_mutex_unlock(&g_lock); runtime_error("thread: 线程表扩容内存不足"); }
        memset(ns + g_cap, 0, (size_t)(newcap - g_cap) * sizeof(ThreadSlot));
        g_slots = ns;
        slot = g_cap;
        g_cap = newcap;
    }
    g_slots[slot].used = 1;
    g_slots[slot].done = 0;
    g_slots[slot].result = val_none();
    g_slots[slot].id = ++g_next_id;
    int tid = g_slots[slot].id;
    pthread_mutex_unlock(&g_lock);

    Value* cargs = (argc > 0) ? (Value*)malloc((size_t)argc * sizeof(Value)) : NULL;
    if(argc > 0 && !cargs) {
        pthread_mutex_lock(&g_lock);
        g_slots[slot].used = 0;
        pthread_mutex_unlock(&g_lock);
        runtime_error("thread: 内存不足");
    }
    for(int i = 0; i < argc; i++) cargs[i] = val_clone(&args[i]);

    ThreadJob* job = (ThreadJob*)malloc(sizeof(ThreadJob));
    if(!job) {
        free(cargs);
        pthread_mutex_lock(&g_lock);
        g_slots[slot].used = 0;
        pthread_mutex_unlock(&g_lock);
        runtime_error("thread: 内存不足");
    }
    job->body = body;
    job->data = data;
    job->slot = slot;
    job->argc = argc;
    job->args = cargs;

    if(pthread_create(&g_slots[slot].handle, NULL, lm_thread_main, job) != 0) {
        pthread_mutex_lock(&g_lock);
        g_slots[slot].used = 0;
        pthread_mutex_unlock(&g_lock);
        free(cargs);
        free(job);
        runtime_error("thread: 创建线程失败");
    }
    return tid;
}

void lumyr_thread_set_result(ThreadLaunch* t, Value r)
{
    /* 工作线程此时可能已 gc_unregister_thread()（VM 通道 vm_run 退出时注销，
     * 编译通道 lm_c_thread_body 在 cf 返回后注销），结果值 r 在 C 栈上不被 GC 扫描。
     * val_clone 期间若其他线程触发 GC，r 引用的堆对象可能被回收 → UAF。
     * 用 gc_protect_push 临时注册保护栈，clone 后 pop。 */
    gc_protect_push(r);
    pthread_mutex_lock(&g_lock);
    g_slots[t->slot].result = val_clone(&r);
    pthread_mutex_unlock(&g_lock);
    gc_protect_pop();
}

void lumyr_thread_set_result_protected(ThreadLaunch* t, Value r)
{
    /* 调用方已 gc_protect_push(r)，此处直接 clone 不再重复保护。
     * 调用方负责随后 gc_protect_pop()。 */
    pthread_mutex_lock(&g_lock);
    g_slots[t->slot].result = val_clone(&r);
    pthread_mutex_unlock(&g_lock);
}

// C 生成端线程体：直接调函数指针
// 修复 root scanning bug：必须在 lumyr_thread_set_result 完成后再 unregister，
// 否则 unregister→protect_push 之间 r 在 C 栈上不被根扫描覆盖，
// 其他线程触发 GC 时 r 引用的堆对象会被错误回收 → UAF。
static void lm_c_thread_body(ThreadLaunch* t)
{
    Value (*cf)(Value*, int) = (Value(*)(Value*, int))t->data;
    /* 根帧兜底：cf 内未捕获的 runtime_error（g_err_jmp 此前为 NULL 直接
     * exit 整个进程）longjmp 到这里——只终止本线程，错误作为结果交给
     * join 重抛。先压 dummy protect 保证返回值始终有 GC 根。 */
    jmp_buf rootpad;
    jmp_buf* volatile prevJmp = g_err_jmp;
    g_err_jmp = &rootpad;
    gc_protect_push(val_none());
    if(setjmp(rootpad) == 0) {
        Value r = cf(t->args, t->argc);
        g_err_jmp = prevJmp;
        gc_protect_set(r);
        lumyr_thread_set_result(t, r);
    } else {
        g_err_jmp = prevJmp;
        Value err = lumyr_make_error(
            (g_err_type && g_err_type[0]) ? g_err_type : "RuntimeError",
            g_err_msg ? g_err_msg : "", NULL);
        gc_protect_set(err);
        lumyr_thread_set_result(t, err);
    }
    gc_protect_pop();
    gc_unregister_cframe_thread();
}

int lumyr_thread_start_c(Value (*cf)(Value*, int), const Value* args, int argc)
{
    return lumyr_thread_start(lm_c_thread_body, (void*)cf, args, argc);
}

Value lumyr_thread_join(int id)
{
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for(int i = 0; i < g_cap; i++) {
        if(g_slots[i].used && g_slots[i].id == id) { slot = i; break; }
    }
    if(slot < 0) {
        pthread_mutex_unlock(&g_lock);
        runtime_error("thread_join: 无效的线程id（不存在或已 join）");
    }
    pthread_t h = g_slots[slot].handle;
    pthread_mutex_unlock(&g_lock);

    /* 进入原生阻塞区：pthread_join 期间不执行 VM 代码、不修改 GC 根，栈稳定。
     * 标记 at_safepoint=1 使工作线程触发的 GC 能立即扫描本线程并继续，
     * 否则主线程卡在 pthread_join 中永远不到达安全点，形成死锁。 */
    gc_enter_native_block();
    pthread_join(h, NULL);   // 等待线程退出（此时 result 已写）
    gc_leave_native_block();

    /* 线程已退出，读取结果（浅拷贝到 C 栈局部变量） */
    pthread_mutex_lock(&g_lock);
    Value r = g_slots[slot].result;
    pthread_mutex_unlock(&g_lock);

    /* val_clone 期间保护 r：r 在 C 栈上不被 GC 扫描，若此时其他线程触发 GC，
     * r 引用的堆对象可能被回收 → UAF。临时注册保护栈。 */
    gc_protect_push(r);
    Value cloned = val_clone(&r);
    gc_protect_pop();

    pthread_mutex_lock(&g_lock);
    g_slots[slot].used = 0;
    g_slots[slot].done = 0;
    pthread_mutex_unlock(&g_lock);
    /* 子线程未捕获错误：join 时在调用线程重新抛出（try 可捕获）。
     * 槽位已释放再跳，longjmp 不泄漏线程表资源。 */
    if(cloned.type == VAL_ERROR) lumyr_rethrow_error(cloned);
    return cloned;
}
