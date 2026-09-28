// lm_ctx_bench.c —— Phase 8.1 切换原语基准
// 两层度量：
//   1. 裸 lm_ctx_jump ping-pong（两个裸上下文互切，隔离纯切换成本）
//   2. lm_co_resume/yield 往返（含 GC 冻结栈注册等真实协程开销）
// 用法：./lm_ctx_bench [迭代次数，默认 10000000]
#include "lm_co.h"
#include "lm_coro_ctx.h"

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static double now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

/* ---- 裸 ctx ping-pong ----
 * bounce 永不返回（切换原语约定：entry 末尾必须显式 jump 切走，
 * 返回即触发 stub 的 ud2 兜底）。测量结束后进程直接退出，不切回清理。 */
static lm_ctx_t g_a, g_b;

static void bounce(void* arg) {
    (void)arg;
    for (;;) lm_ctx_jump(&g_b, &g_a);
}

/* ---- lm_co resume/yield ping-pong ---- */
static void ping(void* arg) {
    (void)arg;
    for (;;) lm_co_yield();
}

int main(int argc, char** argv) {
    long n = (argc > 1) ? atol(argv[1]) : 10000000;

#ifdef LM_CTX_FCONTEXT
    const char* backend = "fcontext";
#else
    const char* backend = "ucontext";
#endif
    printf("backend = %s, iterations = %ld\n", backend, n);

    /* 裸 jump：主上下文 g_a ↔ bounce 上下文 g_b，每轮 2 次切换 */
    size_t stksz = 64 * 1024;
    void* stk = malloc(stksz);   /* bounce 永不返回，栈随进程退出回收 */
    if (!stk) return 1;
    lm_ctx_make(&g_b, stk, stksz, bounce, NULL);
    for (int i = 0; i < 1000; i++) lm_ctx_jump(&g_a, &g_b);   /* 预热 */
    double t0 = now_ns();
    for (long i = 0; i < n; i++) lm_ctx_jump(&g_a, &g_b);
    double t1 = now_ns();
    double raw_ns = (t1 - t0) / (double)(2 * n);
    printf("raw lm_ctx_jump: %.1f ns/switch\n", raw_ns);

    /* lm_co 层：resume+yield 往返 = 2 次切换 + GC 注册/注销 */
    lm_co_t* co = lm_co_spawn(ping, NULL, 0);
    if (!co) return 1;
    for (int i = 0; i < 1000; i++) lm_co_resume(co);
    t0 = now_ns();
    for (long i = 0; i < n; i++) lm_co_resume(co);
    t1 = now_ns();
    double co_ns = (t1 - t0) / (double)(2 * n);
    printf("lm_co resume/yield: %.1f ns/switch (含 GC 冻结栈注册开销)\n", co_ns);
    lm_co_destroy(co);

    printf("ALL PASS (bench done)\n");
    return 0;
}
