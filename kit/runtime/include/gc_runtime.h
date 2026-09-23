#ifndef GC_RUNTIME_H
#define GC_RUNTIME_H

#include <stddef.h>
#include <stdint.h>
#include "lumyr_value_type.h"

/* ============================================================
 * 标记-清除 GC（Mark-Sweep）
 *
 * 所有堆分配的 Value 对象前面统一加 GCObject 头：
 *   用户数据指针 = (char*)obj + sizeof(GCObject)
 *   GCObject*    = (GCObject*)((char*)ptr - sizeof(GCObject))
 *
 * vtype 取值为 VAL_STRING/VAL_ARRAY/VAL_MAP/VAL_ERROR/VAL_FUNC，
 * 与 ValueType 枚举一致，用于标记阶段递归遍历内部引用。
 * ============================================================ */

/* 分代 GC：对象年龄阈值，达到该值后从新生代晋升老年代 */
#define PROMOTE_AGE 3

/* GCObject flags 位定义 */
#define GC_OBJ_IN_RS 0x01  /* bit 0：对象已在 remembered set 中（去重） */

typedef struct GCObject {
    unsigned char marked;
    unsigned char vtype;      /* VAL_STRING / VAL_ARRAY / VAL_MAP / VAL_ERROR / VAL_FUNC */
    unsigned char age;         /* 分代年龄：<PROMOTE_AGE=新生代，>=PROMOTE_AGE=老年代 */
    unsigned char flags;       /* 标志位：bit0=GC_OBJ_IN_RS, bit1=GC_OBJ_INTERNAL_BUF（items/buckets 内部缓冲区，非用户对象） */
    uint32_t user_size;       /* 用户数据大小（不含 GCObject 头） */
    struct GCObject* next;
} GCObject;

/* ---- 分配 / 重分配 ---- */
/* 分配 size + sizeof(GCObject)，初始化头，挂入全局链表，返回用户数据指针 */
void* gc_alloc(size_t size, int vtype);

/* 从链表移除旧节点，realloc，重新挂入，返回新用户数据指针。
 * 只用于 realloc 数组 items 等内部缓冲区（ValueArray* 本身不变，只有 items 指针变） */
void* gc_realloc(void* ptr, size_t new_size);

/* 老年代分配：分配后直接设 age=PROMOTE_AGE，用于内部缓冲区
 * （items/buckets/tree/MapEntry），避免被老年代容器引用时 Minor GC 错误回收 */
void* gc_alloc_old(size_t size, int vtype);

/* 读 GC 对象头的运行时 vtype（ptr 须为 gc_alloc 返回的用户指针）；
 * 指针无效返回 -1。供 BOX_PTR 装箱按运行时身份打标（修复字段声明类型
 * 与运行时容器类型不一致时身份丢失）。 */
int gc_obj_vtype(void* ptr);

/* Remembered set 检查：老年代容器写入新值时，若新值可能引用新生代对象，
 * 将容器加入 remembered set。在所有修改堆对象内部引用的位置调用。 */
void gc_remembered_set_check(Value owner, Value new_val);

/* 返回 remembered set 当前大小（统计用） */
size_t gc_rs_size(void);

/* ---- 标记 ---- */
/* 如果 v 是堆类型，标记其 GCObject，然后递归标记内部引用 */
void gc_mark(Value v);

/* 标记原始 GC 指针（用于 buckets/tree/MapEntry 等内部缓冲区，不递归 Value）
 * 返回 1=新标记（需递归子对象），0=已标记/永生/空 */
int gc_mark_ptr(void* ptr);

/* 遍历 VM 栈 stack[0..sp-1]、当前帧及父帧链的局部变量 */
void gc_mark_roots(Value* stack, int sp, StackFrame* frame);

/* ---- 清除 / 回收 ---- */
/* 遍历 g_gc_objects，未标记的释放，清除标记位 */
void gc_sweep(void);

/* mark_roots → sweep → 更新阈值（分代 GC 调度入口：启用时触发 Minor，禁用时触发 Major） */
void gc_collect(Value* stack, int sp, StackFrame* frame);

/* Major GC：全量增量标记-清除（sweep 全部对象，age 不变） */
void gc_collect_major(Value* stack, int sp, StackFrame* frame);

/* Minor GC：全量 STW 标记新生代 + remembered set，只 sweep 新生代
 * （存活对象 age++，达到阈值晋升老年代，晋升对象加入 remembered set） */
void gc_collect_minor(Value* stack, int sp, StackFrame* frame);

/* ---- 根注册（供 gc_alloc 内部触发 GC 时使用） ---- */
/* VM 执行循环入口调用，注册当前线程的栈/帧；退出时置 NULL */
void gc_set_roots(Value* stack, int* sp_ptr, StackFrame* frame);

/* 获取当前注册的根（用于嵌套 vm_run 保存/恢复） */
void gc_get_roots(Value** stack, int** sp_ptr, StackFrame** frame);

/* ---- GC 暂停/恢复（构造复合对象时防止中间态被 sweep） ---- */
/* 暂停自动 GC（计数器可嵌套）；构造多个 GC 对象期间调用，
   避免第一个对象尚未被根引用时，第二个对象的分配触发 GC 将其回收 */
void gc_disable(void);
/* 恢复自动 GC；计数器归零时若超过阈值则触发一次 GC */
void gc_enable(void);

/* 手动触发一次 GC（使用当前注册的根） */
void gc_collect_now(void);

/* 钉住对象：标记为永生，GC 永不回收（用于常量表中的字符串/数组等） */
void gc_pin(void* ptr);

/* ---- 多线程栈根注册表 ---- */
/* 注册当前线程的操作数栈/栈指针/帧链指针到全局注册表。
 * GC 时遍历所有注册线程，扫描每个线程的 stack[0..*sp] + frame 链局部变量。
 * sp_ptr 和 frame 是指针（动态变化），GC 时解引用获取当前值。
 * 线程退出时必须调用 gc_unregister_thread()。
 * 支持同一线程嵌套注册（栈式语义），unregister 移除最近注册的 entry。 */
void gc_register_thread(Value* stack, int* sp_ptr, StackFrame* frame);

/* 从全局注册表移除当前线程最近注册的 entry（栈式语义） */
void gc_unregister_thread(void);

/* 移除当前线程倒数第二个 entry（保留最近注册的 protect entry）。
 * 用于 OPC_RETURN 最外层：先 gc_protect_push(v)，再调本函数移除 VM entry，
 * 实现 protect 与 unregister 无窗口衔接。 */
void gc_unregister_thread_keep_protect(void);

/* 注册全局根扫描回调：外部模块（如线程表）注册需 GC 扫描的全局堆引用。
 * GC 标记阶段会调用此回调，确保全局表中的活跃堆对象不被误回收。 */
typedef void (*GCGlobalRootScanFn)(void);
void gc_register_global_root_scan(GCGlobalRootScanFn fn);

/* 协作式 STW 安全点：VM 解释循环每条指令前调用，GC 运行时自旋等待 */
void gc_stw_check(void);

/* STW 全局标志：GC 运行时置 1 通知所有线程暂停，置 0 恢复。
 * 暴露为 extern 以供 gc_stw_check_fast() 内联快速路径使用，
 * 避免非 GC 时每次检查都产生函数调用开销。 */
extern volatile int g_gc_stw;

/* 内联 STW 快速路径：非 GC 时仅一次 volatile 读 + 分支（通常预测不跳转），
 * 无函数调用开销；GC 时才调用 gc_stw_check() 进入自旋等待。
 * 编译通道生成的 C 代码和 VM 解释循环应优先调用此函数。 */
static inline void gc_stw_check_fast(void) {
    if (!g_gc_stw) return;
    gc_stw_check();
}

/* ---- 原生阻塞区（Native Block）----
 * 线程在原生阻塞调用（pthread_join / pthread_mutex_lock / pthread_cond_wait /
 * usleep 等）中不执行 VM 代码、不修改 GC 根，操作数栈与帧链稳定，可视为在安全点。
 *
 * gc_enter_native_block()：将当前线程所有注册 entry（VM + CFrame）的 at_safepoint 置 1，
 *   使正在等待的 GC 能立即扫描本线程栈并继续；随后进入原生阻塞调用。
 * gc_leave_native_block()：原生调用返回后将 at_safepoint 恢复为 0。
 *
 * 必须严格配对使用（enter → 原生阻塞调用 → leave），且 enter 与 leave 之间不得执行
 * 任何可能修改 GC 根的 VM 级操作（如分配、写入堆对象字段）。
 * 与 gc_stw_check() 的区别：gc_stw_check 会自旋等待 GC 结束，而 native_block 只标记
 * 安全点后立即返回（线程本身已被原生调用阻塞，无需额外自旋）。 */
void gc_enter_native_block(void);
void gc_leave_native_block(void);

/* ---- GC 值保护（Value Protect）----
 * 临时将一个 Value 注册为 GC 根，使其在原生阻塞区/线程退出等场景下不被误回收。
 * 典型场景：工作线程退出时已 gc_unregister_thread()，结果值在 C 栈上不被 GC 扫描；
 * 此时若其他线程触发 GC，结果值引用的堆对象可能被回收 → val_clone 时 UAF。
 * 在 val_clone 前调用 gc_protect_push(r)，clone 后 gc_protect_pop()。
 * 必须严格配对；内部使用 TLS 保存旧根，不可嵌套调用。 */
void gc_protect_push(Value v);
void gc_protect_pop(void);
void gc_protect_set(Value v);
void gc_mark_internal_buf(void* ptr);

/* ============================================================
 * 编译通道（C 代码生成）帧链表
 *
 * 编译通道的局部变量分散在 C 栈上，无法像 VM 那样通过 StackFrame 统一遍历。
 * 每个函数入口 push 一个 CFrame（含操作数栈 + 局部变量指针数组），出口 pop。
 * GC 标记时解引用 local_ptrs 扫描当前值，同时扫描操作数栈。
 * ============================================================ */
typedef struct CFrame {
    Value* stack;          /* 函数操作数栈 __stk */
    int* sp;               /* 指向栈顶计数器 __sp */
    int stack_size;        /* __stk 数组容量（元素数），用于保守式扫描跳过已精确扫描的区域 */
    Value** local_ptrs;    /* 局部变量地址数组（参数 + 局部 + 标量替换变量） */
    int nlocals;           /* local_ptrs 有效元素数 */
    struct CFrame* parent; /* 调用者帧 */
} CFrame;

/* 注册/注销当前编译通道帧（TLS 链表）。
 * push：f->parent = 当前顶; 当前顶 = f; 同时设置 tls_stack/tls_sp 使自动 GC 条件成立；
 *       首次 push 时自动将当前线程注册到 CFrame 线程注册表。
 * pop：当前顶 = parent; 恢复 tls_stack/tls_sp 为父帧的值（或 NULL）。 */
void gc_push_cframe(CFrame* f);
void gc_pop_cframe(void);

/* 获取/恢复当前 CFrame 链顶（用于 try/catch longjmp 后恢复帧链） */
CFrame* gc_cframe_top(void);
void gc_cframe_restore(CFrame* top);

/* 注册/注销当前线程的 CFrame 链到全局注册表（多线程 GC 扫描所有线程）。
 * gc_push_cframe 首次调用时自动注册；线程退出时需显式 unregister。 */
void gc_register_cframe_thread(void);
void gc_unregister_cframe_thread(void);

/* 统计 */
size_t gc_bytes(void);
size_t gc_count(void);
size_t gc_young_bytes(void);
size_t gc_old_bytes(void);

/* ============================================================
 * 增量标记（Incremental Marking）—— 三色标记 + 写屏障
 *
 * marked 值定义：
 *   0 = 白色（未访问）
 *   1 = 黑色（已处理，子对象已全部标记）
 *   2 = 永生（pin / 空闲链表预分配，不变）
 *   3 = 新对象预标记（非标记期分配，标记期开始时视同白色需遍历）
 *   4 = 灰色（已访问，子对象未处理，在标记栈中）
 *
 * 标记栈：全局灰色对象队列，GC 线程 pop 处理，应用线程写屏障 push。
 * 内部缓冲区（buckets/tree/MapEntry）与 ValueMap 共用 vtype=VAL_MAP，
 * 因此内部缓冲区直接标记为黑色（不入灰色栈），由父级 ValueMap 的
 * gc_mark_one 显式扫描其 key/value 子引用。
 * ============================================================ */

/* 并发标记阶段标志：1=写屏障生效，0=写屏障空操作 */
extern volatile int g_gc_marking;

/* 写屏障实现（gc_runtime.c）：对新值中的白色堆对象变灰入栈 */
void gc_write_barrier_impl(Value new_val);

/* Dijkstra 风格写屏障：并发标记期间，若新值引用白色堆对象，将其变灰入标记栈，
 * 防止黑色对象引用白色对象破坏三色不变式。
 * 内联快速路径：非标记期仅一次 volatile 读 + 分支，无函数调用开销。 */
static inline void gc_write_barrier(Value new_val) {
    if (!g_gc_marking) return;
    gc_write_barrier_impl(new_val);
}

/* 迭代式标记：将白色/预标记对象变灰入标记栈（返回 1=新入栈，0=已处理/永生/空） */
int gc_mark_ptr_to_stack(void* ptr);

/* 对 Value 中的堆对象调用 gc_mark_ptr_to_stack；stack_alloc 容器直接扫描子元素 */
void gc_mark_value_to_stack(Value v);

/* 处理一个灰色对象的子对象（白色子对象变灰入栈），完成后自身设为黑色 */
void gc_mark_one(GCObject* obj);

/* 扫描所有线程根（VM 栈 + 帧链 + CFrame 链），白色对象变灰入栈 */
void gc_scan_roots_to_stack(Value* stack, int sp, StackFrame* frame);

/* 累计 STW 停顿时间（纳秒），用于性能验证 */
unsigned long long gc_stw_time_ns(void);

#endif /* GC_RUNTIME_H */
