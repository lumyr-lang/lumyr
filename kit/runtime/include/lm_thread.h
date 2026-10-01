// lm_thread.h —— 多线程：thread(f, args...) 启动线程 / thread_join(tid) 等待并取返回值
// 机制：通用线程启动 + 调用方提供线程体（ThreadBody）
//   - C 生成端：lumyr_thread_start_c 内置 C 线程体（直接调 Value(*)(Value*,int)）
//   - VM 端：vm.c 提供 vm_thread_body（interp_set_current_rf + entry），调 lumyr_thread_start
// 语义：
//   - thread 返回自增线程 id（int），线程 joinable，join 后槽位回收
//   - 线程函数返回值经 thread_join 获取；不 join 的线程退出后占槽；线程表动态扩容，无硬上限
//   - 参数按值克隆（字符串/数组深拷贝，函数为引用语义），线程内独立执行器状态（_Thread_local）
//   - 线程内未捕获错误与主线程一致：runtime_error → 进程退出；推荐在线程函数内 try/catch
//   - 脚本全局变量（lmvar_*）跨线程共享，竞态由用户负责（多线程固有语义）
#ifndef LM_THREAD_H
#define LM_THREAD_H

#include "lumyr_value_type.h"

#define LM_THREAD_INITIAL_CAP 64   // 线程表初始容量；按需翻倍扩容，无硬上限（受系统资源/OS 限制）

// 线程启动上下文（线程体内使用）：args 为克隆参数（线程退出后由 lm_thread 释放）
typedef struct {
    int slot;       // 槽位（内部）
    int argc;
    Value* args;    // 克隆的参数数组
    void* data;     // 调用方数据（C 端=函数指针；VM 端=RuntimeFunc*）
} ThreadLaunch;

typedef void (*ThreadBody)(ThreadLaunch*);   // 线程体：执行函数调用并调用 lumyr_thread_set_result

int lumyr_thread_start(ThreadBody body, void* data, const Value* args, int argc);
int lumyr_thread_start_c(Value (*cf)(Value*, int), const Value* args, int argc);
Value lumyr_thread_join(int id);
/* 分离线程：不关心返回值，线程退出时立即回收线程表槽位（长跑服务的即发即忘任务用）。
 * 语义：detach 后 tid 失效，不可再 join；线程已退出则当场回收。幂等失败按无效 id 报错。 */
int lumyr_thread_detach(int id);
void lumyr_thread_set_result(ThreadLaunch* t, Value r);
/* 已保护版本：调用方已用 gc_protect_push(r) 保护 r，本函数不再重复保护。
 * 用于 VM 通道线程体：vm_run 已自行 unregister，需在 cleanup 前 protect_push(r)。 */
void lumyr_thread_set_result_protected(ThreadLaunch* t, Value r);

#endif // LM_THREAD_H
