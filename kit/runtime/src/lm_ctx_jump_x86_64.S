/* lm_ctx_jump_x86_64.S —— fcontext 协程切换（System V AMD64，macOS/Linux 通用）
 * 纯用户态，只做三件事：
 *   1. 保存 callee-saved 寄存器到当前栈（rbp/rbx/r12-r15 + 返回地址）
 *   2. 交换栈指针（from->sp = rsp; rsp = to->sp）
 *   3. 恢复目标寄存器后 ret 跳入
 * 不保存 FPU/SIMD（System V 全 caller-saved），不动信号掩码（协程共享线程信号上下文）。
 *
 * 帧布局约定（与 lm_ctx.c 的 lm_ctx_make 严格一致，改动必须双向同步）：
 *   ctx->sp 指向 7 槽帧的最低地址：[r15 r14 r13 r12(arg) rbx(entry) rbp ret]
 * 首次切入经 lm_ctx_start stub：r12→rdi（arg）、rbx→rax（entry），对齐后 call。
 *
 * 本文件经 C 预处理器处理：-DLM_CTX_UCONTEXT 回退构建时整体为空，
 * 避免与 lm_ctx.c 的 C 版 lm_ctx_jump 重定义。 */
#if !defined(LM_CTX_UCONTEXT)

#if defined(__APPLE__)
#define C_SYMBOL(name) _##name
    .section __TEXT,__text,regular,pure_instructions
#else
#define C_SYMBOL(name) name
    .text
#endif

    .intel_syntax noprefix

/* void lm_ctx_jump(lm_ctx_t* from [rdi], lm_ctx_t* to [rsi]) */
    .p2align 4
    .globl C_SYMBOL(lm_ctx_jump)
#if !defined(__APPLE__)
    .type lm_ctx_jump, @function
#endif
C_SYMBOL(lm_ctx_jump):
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov  [rdi], rsp        /* from->sp = 保存点（含 call 压入的返回地址） */
    mov  rsp, [rsi]        /* 切到目标协程栈 */
    pop  r15
    pop  r14
    pop  r13
    pop  r12
    pop  rbx
    pop  rbp
    ret                    /* 弹目标返回地址并跳入（make 帧：lm_ctx_start） */
#if !defined(__APPLE__)
    .size lm_ctx_jump, .-lm_ctx_jump
#endif

/* 初始帧 stub：make 伪造帧的返回地址指向这里。
 * 约定：r12=arg、rbx=entry；rbp 清 0 终止帧链。
 * entry 不得返回（co_trampoline 末尾显式 lm_ctx_jump），ud2 兜底。 */
    .p2align 4
C_SYMBOL(lm_ctx_start):
    mov  rdi, r12          /* arg → 第 1 实参 */
    mov  rax, rbx          /* entry */
    xor  ebp, ebp          /* 帧链终止 */
    and  rsp, -16          /* call 前 16B 对齐（ABI 要求） */
    call rax
    ud2

/* void* lm_ctx_start_ptr(void)：返回 stub 地址。
 * 汇编码内 rip 相对取址，规避 arm64e PAC 式函数指针签名问题（双架构统一接口）。 */
    .p2align 4
    .globl C_SYMBOL(lm_ctx_start_ptr)
#if !defined(__APPLE__)
    .type lm_ctx_start_ptr, @function
#endif
C_SYMBOL(lm_ctx_start_ptr):
    lea  rax, [rip + C_SYMBOL(lm_ctx_start)]
    ret
#if !defined(__APPLE__)
    .size lm_ctx_start_ptr, .-lm_ctx_start_ptr
    .section .note.GNU-stack,"",@progbits   /* 声明非可执行栈（ELF） */
#endif

#endif /* !LM_CTX_UCONTEXT */
