/* lm_ctx_jump_arm64.S —— fcontext 协程切换（AAPCS64，macOS Apple Silicon / Linux arm64）
 * 纯用户态，只做三件事：
 *   1. 保存 callee-saved 寄存器到当前栈（x19-x29、x30/lr、d8-d15）
 *   2. 交换栈指针（from->sp = sp; sp = to->sp）
 *   3. 恢复目标寄存器后 ret 跳入
 * d8-d15 是 AAPCS64 callee-saved（NEON 低 64 位），必须保存——与 x86_64 的关键差异。
 * x18 为平台保留寄存器，本例程不触碰。
 *
 * PAC（arm64e 指针签名）说明：
 *   - 保存的 lr 是 bl 指令写入的原始返回地址（未签名），ret 不鉴权，直接恢复安全；
 *   - make 伪造帧的 lr 槽由 lm_ctx_start_ptr 以 adrp/add 取址填入（未签名），
 *     规避 C 侧取函数地址可能带签名的问题。
 *
 * 帧布局约定（与 lm_ctx.c 的 lm_ctx_make 严格一致，改动必须双向同步）：
 *   ctx->sp 指向 160B 帧最低地址：
 *   [x19(arg) x20(entry) x21..x28(0) x29(fp=0) x30(lr=lm_ctx_start) d8..d15(0)]
 * 首次切入：jump 末尾 ret 跳入 lm_ctx_start，x19→x0（arg）、x20→x9（entry），blr。
 *
 * 本文件经 C 预处理器处理：-DLM_CTX_UCONTEXT 回退构建时整体为空。 */
#if !defined(LM_CTX_UCONTEXT)

#if defined(__APPLE__)
#define C_SYMBOL(name) _##name
    .section __TEXT,__text,regular,pure_instructions
#else
#define C_SYMBOL(name) name
    .text
#endif

/* void lm_ctx_jump(lm_ctx_t* from [x0], lm_ctx_t* to [x1]) */
    .p2align 4
    .globl C_SYMBOL(lm_ctx_jump)
#if !defined(__APPLE__)
    .type lm_ctx_jump, @function
#endif
C_SYMBOL(lm_ctx_jump):
    sub  sp, sp, #160
    stp  x19, x20, [sp, #0]
    stp  x21, x22, [sp, #16]
    stp  x23, x24, [sp, #32]
    stp  x25, x26, [sp, #48]
    stp  x27, x28, [sp, #64]
    stp  x29, x30, [sp, #80]    /* fp, lr（lr = bl 写入的原始返回地址） */
    stp  d8,  d9,  [sp, #96]
    stp  d10, d11, [sp, #112]
    stp  d12, d13, [sp, #128]
    stp  d14, d15, [sp, #144]
    mov  x9, sp
    str  x9, [x0]               /* from->sp = 保存点 */
    ldr  x9, [x1]               /* 切到目标协程栈 */
    mov  sp, x9
    ldp  x19, x20, [sp, #0]
    ldp  x21, x22, [sp, #16]
    ldp  x23, x24, [sp, #32]
    ldp  x25, x26, [sp, #48]
    ldp  x27, x28, [sp, #64]
    ldp  x29, x30, [sp, #80]
    ldp  d8,  d9,  [sp, #96]
    ldp  d10, d11, [sp, #112]
    ldp  d12, d13, [sp, #128]
    ldp  d14, d15, [sp, #144]
    add  sp, sp, #160
    ret                         /* 跳入 lr（make 帧：lm_ctx_start） */
#if !defined(__APPLE__)
    .size lm_ctx_jump, .-lm_ctx_jump
#endif

/* 初始帧 stub：x19=arg、x20=entry；x29 清 0 终止帧链。
 * 进入时 sp = frame+160 ≡ 0 (mod 16)，满足 AAPCS64 公共接口对齐，无需调整。
 * entry 不得返回（co_trampoline 末尾显式 lm_ctx_jump），brk 兜底。 */
    .p2align 4
C_SYMBOL(lm_ctx_start):
    mov  x0, x19           /* arg → 第 1 实参 */
    mov  x9, x20           /* entry */
    mov  x29, xzr          /* 帧链终止 */
    blr  x9
    brk  #0x1

/* void* lm_ctx_start_ptr(void)：返回 stub 地址（未签名原始地址，PAC 安全）。 */
    .p2align 4
    .globl C_SYMBOL(lm_ctx_start_ptr)
#if !defined(__APPLE__)
    .type lm_ctx_start_ptr, @function
#endif
C_SYMBOL(lm_ctx_start_ptr):
#if defined(__APPLE__)
    adrp x0, C_SYMBOL(lm_ctx_start)@PAGE
    add  x0, x0, C_SYMBOL(lm_ctx_start)@PAGEOFF
#else
    adrp x0, C_SYMBOL(lm_ctx_start)
    add  x0, x0, :lo12:C_SYMBOL(lm_ctx_start)
#endif
    ret
#if !defined(__APPLE__)
    .size lm_ctx_start_ptr, .-lm_ctx_start_ptr
    .section .note.GNU-stack,"",@progbits   /* 声明非可执行栈（ELF） */
#endif

#endif /* !LM_CTX_UCONTEXT */
