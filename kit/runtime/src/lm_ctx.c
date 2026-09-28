// lm_ctx.c —— 协程上下文切换原语实现（Phase 8.1）
// fcontext 后端：lm_ctx_jump 在汇编（lm_ctx_jump_x86_64.S / lm_ctx_jump_arm64.S），
// 本文件实现 lm_ctx_make（在栈顶伪造初始帧，布局必须与汇编的保存/恢复序列逐槽一致）。
// ucontext 后端：两个原语都在本文件（-DLM_CTX_UCONTEXT 排障对照用）。
#include "lm_coro_ctx.h"

#include <stdlib.h>
#include <string.h>

#if defined(LM_CTX_FCONTEXT)

/* 初始帧 trampoline 的地址由汇编侧给出（adrp/add 直接取址）：
 * arm64e（PAC）下 C 侧 &func 取函数地址可能带签名，直接写进伪造帧再
 * 被汇编裸分支使用会触发鉴权失败；由汇编取址则天然是未签名原始地址。
 * x86_64 无 PAC，同样走此接口保持两架构代码路径统一。 */
extern void* lm_ctx_start_ptr(void);

#if defined(__x86_64__)
/* ============ x86_64 帧布局（与 lm_ctx_jump_x86_64.S 严格一致） ============
 * jump 保存序列：push rbp/rbx/r12/r13/r14/r15（call 已压返回地址），
 * 恢复序列：pop r15/r14/r13/r12/rbx/rbp + ret（弹返回地址跳入）。
 * 伪造帧从 ctx->sp（低地址）向高地址共 7 槽：
 *   [0] r15 = 0
 *   [1] r14 = 0
 *   [2] r13 = 0
 *   [3] r12 = arg            （stub 装入 rdi）
 *   [4] rbx = entry          （stub call 目标）
 *   [5] rbp = 0              （帧链终止：GC 保守扫描不依赖帧链，此为调试器友好）
 *   [6] 返回地址 = lm_ctx_start（asm stub）
 * 任何改动必须与 .S 文件同步修改！ */
#define LM_X86_64_FRAME_WORDS 7

void lm_ctx_make(lm_ctx_t* ctx, void* sp, size_t size,
                 void (*entry)(void*), void* arg) {
    /* 栈顶（高地址）16B 对齐后向下放帧 */
    uintptr_t top = ((uintptr_t)sp + size) & ~(uintptr_t)15;
    uintptr_t* frame =
        (uintptr_t*)(top - LM_X86_64_FRAME_WORDS * sizeof(uintptr_t));
    frame[0] = 0;
    frame[1] = 0;
    frame[2] = 0;
    frame[3] = (uintptr_t)arg;
    frame[4] = (uintptr_t)entry;
    frame[5] = 0;
    frame[6] = (uintptr_t)lm_ctx_start_ptr();
    ctx->sp = (void*)frame;
}

#elif defined(__aarch64__)
/* ============ arm64 帧布局（与 lm_ctx_jump_arm64.S 严格一致） ============
 * jump 保存序列（160B 帧）：stp x19-x29/x30 于 [sp,#0..#95]，d8-d15 于 [#96..#159]。
 * 伪造帧从 ctx->sp（低地址）向高地址共 20 槽（160B，保持 sp 16B 对齐）：
 *   [0]  x19 = arg           （stub 装入 x0）
 *   [1]  x20 = entry         （stub blr 目标）
 *   [2..9]  x21..x28 = 0
 *   [10] x29(fp) = 0         （帧链终止）
 *   [11] x30(lr) = lm_ctx_start（asm stub；jump 末尾 ret 跳入）
 *   [12..19] d8..d15 = 0
 * 任何改动必须与 .S 文件同步修改！ */
#define LM_ARM64_FRAME_BYTES 160

void lm_ctx_make(lm_ctx_t* ctx, void* sp, size_t size,
                 void (*entry)(void*), void* arg) {
    uintptr_t top = ((uintptr_t)sp + size) & ~(uintptr_t)15;
    uintptr_t* frame = (uintptr_t*)(top - LM_ARM64_FRAME_BYTES);
    memset(frame, 0, LM_ARM64_FRAME_BYTES);
    frame[0]  = (uintptr_t)arg;
    frame[1]  = (uintptr_t)entry;
    frame[11] = (uintptr_t)lm_ctx_start_ptr();
    ctx->sp = (void*)frame;
}
#endif

#else /* ==================== ucontext 回退后端 ==================== */

void lm_ctx_jump(lm_ctx_t* from, lm_ctx_t* to) {
    /* swapcontext 语义与 lm_ctx_jump 对称语义天然一致 */
    swapcontext(&from->uc, &to->uc);
}

/* makecontext 只能传 int 参数：entry/arg 各拆 hi/lo 两截，stub 内拼回。
 * entry 约定不得返回（co_trampoline 末尾显式 lm_ctx_jump 切走），
 * 返回即 bug，abort 兜底（等价 fcontext stub 的 trap）。 */
static void lm_ctx_uc_trampoline(unsigned int ehi, unsigned int elo,
                                 unsigned int ahi, unsigned int alo) {
    void (*entry)(void*) =
        (void (*)(void*))(((uintptr_t)ehi << 32) | (uintptr_t)elo);
    void* arg = (void*)(((uintptr_t)ahi << 32) | (uintptr_t)alo);
    entry(arg);
    abort();
}

void lm_ctx_make(lm_ctx_t* ctx, void* sp, size_t size,
                 void (*entry)(void*), void* arg) {
    getcontext(&ctx->uc);
    ctx->uc.uc_stack.ss_sp = sp;
    ctx->uc.uc_stack.ss_size = size;
    ctx->uc.uc_link = NULL;   /* 隐式链回已废弃：trampoline 末尾显式 jump */
    uintptr_t e = (uintptr_t)entry;
    uintptr_t a = (uintptr_t)arg;
    makecontext(&ctx->uc, (void (*)(void))lm_ctx_uc_trampoline, 4,
                (unsigned int)(e >> 32), (unsigned int)(e & 0xFFFFFFFFu),
                (unsigned int)(a >> 32), (unsigned int)(a & 0xFFFFFFFFu));
}

#endif /* LM_CTX_FCONTEXT */
