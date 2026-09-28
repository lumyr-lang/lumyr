// lm_coro_ctx.h —— 协程上下文切换原语抽象层（Phase 8.1）
// 设计目标：把切换机制从 lm_co.c 中剥离，默认 fcontext 纯用户态汇编切换
// （对标 bthread bthread_jump_fcontext / libco coctx_swap），
// 保留 ucontext 后端作为排障对照（-DLM_CTX_UCONTEXT 一行回退）。
//
// 两后端语义完全对齐：
//   lm_ctx_make  在指定栈上构造初始上下文，首次切入从 entry(arg) 开始执行；
//   lm_ctx_jump  保存当前上下文到 *from、恢复 *to 继续执行（对称语义：
//                本调用在"他人 jump 回 *from"时返回，对调用方表现为普通函数返回）。
//
// 与 ucontext 的行为差异（有意为之，生产级取舍）：
//   - 不保存/恢复信号掩码：协程共享线程信号上下文（省 rt_sigprocmask 系统调用）；
//   - 不保存 FPU/SIMD：System V AMD64 下全为 caller-saved；AAPCS64 下 d8-d15
//     为 callee-saved，arm64 汇编已保存；
//   - errno 是 TLS，天然随线程保留，两后端语义一致（不做 bthread 式协程私有 errno）。
#ifndef LM_CORO_CTX_H
#define LM_CORO_CTX_H

/* macOS ucontext 标记 deprecated 但仍可用；需定义 _XOPEN_SOURCE 才能 include ucontext.h。
 * 同时定义 _DARWIN_C_SOURCE 让 BSD 符号（如 MAP_ANON）可见，否则 _XOPEN_SOURCE 会隐藏。
 * 原定义在 lm_co.h 顶部，所有经 lm_co.h 传递依赖本宏的编译单元行为不变。 */
#ifdef __APPLE__
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#endif

#include <stddef.h>
#include <stdint.h>

/* 后端选择：默认 fcontext（x86_64 / arm64 有配套汇编实现）；
 * -DLM_CTX_UCONTEXT 强制回退 POSIX ucontext（排障对照）。
 * Windows（无 ucontext、ABI 不同）暂不支持本切换层。 */
#if !defined(LM_CTX_UCONTEXT) && !defined(_WIN32) && \
    (defined(__x86_64__) || defined(__aarch64__))
#define LM_CTX_FCONTEXT 1
#endif

#if defined(LM_CTX_FCONTEXT)
/* fcontext：寄存器现场保存在协程自己的栈上（lm_ctx_make 伪造初始帧 /
 * lm_ctx_jump 保存帧），上下文结构仅需一个栈指针槽。
 * 16 字节自然对齐（void* 槽），满足"不透明对齐槽位"约定。 */
typedef struct lm_ctx_s {
    void* sp;   /* 冻结协程的栈指针；寄存器现场在栈上 */
} lm_ctx_t;
#else
/* ucontext 回退：嵌入完整 ucontext_t（含信号掩码 + FPU 态，重但通用）。 */
#include <ucontext.h>
typedef struct lm_ctx_s {
    ucontext_t uc;
} lm_ctx_t;
#endif

/* 切换：保存当前上下文到 *from，恢复 *to 并继续执行。
 * 从 *from 视角看：本调用不立即返回，等"他人 jump 回 *from"时才返回。 */
void lm_ctx_jump(lm_ctx_t* from, lm_ctx_t* to);

/* 在栈 [sp, sp+size) 上构造初始上下文（sp=可用区低地址，栈向低地址生长，
 * 与 mmap 分配语义一致）。首次 jump 到本 ctx 时从 entry(arg) 开始执行。
 * 约定：entry 末尾必须显式 lm_ctx_jump 切走、不得返回（asm stub 内置 trap
 * 兜底；ucontext 后端 trampoline 置 abort 兜底），uc_link 式隐式链回已废弃。 */
void lm_ctx_make(lm_ctx_t* ctx, void* sp, size_t size,
                 void (*entry)(void*), void* arg);

#endif /* LM_CORO_CTX_H */
