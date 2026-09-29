// lm_stack_pool.h —— Phase 8.6 B：per-thread 栈池
// 借鉴 bthread StackFactory（brpc stack_inl.h:124-161）：per-thread 对象池，
// get_object 取池 + ASAN unpoison，return_object poison + 归还池。
// 池上限对齐 bthread tc_stack_small=32 / tc_stack_normal=8（stack.cpp:38-39）。
//
// 设计：
//   - per-thread 两桶 free list（SMALL 16KiB / NORMAL 128KiB），无跨线程锁。
//   - intrusive free list：归还的栈，其可用区首 word 存 next 指针（in-band），
//     无额外节点 malloc。get 时读 next、出栈；return 时写 next、入栈。
//   - 池满则 munmap 归还 OS；池空则 mmap+guard 分配新栈。
//   - ASAN：return 时 poison 整个可用区（标记"已释放"），get 时 unpoison
//     （标记"活跃"），与 bthread stack_inl.h:151-158 配对语义一致。
//   - 线程退出时 pthread_key 析构 munmap 拋留栈。
//
// 调用方（lm_co.c）：spawn 时 get，destroy 时 return。消除高频 spawn/destroy
// 的 mmap/munmap syscall 抖动。
#ifndef LM_STACK_POOL_H
#define LM_STACK_POOL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 栈存储描述：get 填充、return 读取。
 * mmap_base：低地址，munmap 用；stack_base：高地址，栈基址（可用区末）；
 * stack_top：低地址，可用区起；stack_size：可用区大小（不含 guard）；
 * total：mmap 总大小（stack_size + guard_page）。 */
typedef struct lm_stack_storage {
    void*  mmap_base;
    void*  stack_base;
    void*  stack_top;
    size_t stack_size;
    size_t total;
} lm_stack_storage_t;

/* 从 per-thread 池取栈。stack_class 选桶（LM_STACK_CLASS_SMALL/NORMAL）。
 * 池命中：unpoison + 出栈；池空：mmap + guard 分配新栈。
 * 成功返回 0 并填充 *out；失败（mmap 失败）返回 -1。 */
int lm_stack_pool_get(int stack_class, lm_stack_storage_t* out);

/* 归还栈到 per-thread 池。st 字段与 get 返回时一致。
 * poison 整个可用区 + 入栈（未满）或 munmap（池满）。
 * st 可为栈上局部变量，return 内部拷贝字段。 */
void lm_stack_pool_return(const lm_stack_storage_t* st);

/* 诊断：返回 per-thread 当前池缓存数（class 选桶）。测试用。 */
int lm_stack_pool_count(int stack_class);

#ifdef __cplusplus
}
#endif

#endif /* LM_STACK_POOL_H */
