// lm_math.c —— 数学内置函数
#include "lm_math.h"
#include "lm_reactor.h"   /* lm_now_ns：单调时钟纳秒，随机数播种熵源 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <pthread.h>



/* 辅助函数：判断是否为数字类型（包括所有整数类型、浮点类型、字符类型） */
static int is_numeric_type(ValueType t) {
    switch(t) {
        case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
        case VAL_BYTE: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT64:
        case VAL_LONG: case VAL_ULONG: case VAL_SIZE_T: case VAL_SSIZE_T:
        case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE:
        case VAL_BOOL: case VAL_CHAR:
            return 1;
        default:
            return 0;
    }
}

static Value num_round(Value x, int up)
{
    if(!is_numeric_type(x.type))
        runtime_error("参数必须是数字");
    double d = value_as_number(x);
    double r = up ? ceil(d) : floor(d);
    if(r > 9.2e18 || r < -9.2e18)
        runtime_error("取整结果超出整数范围");
    return lumyr_make_int((long long)r);
}

Value lumyr_floor(Value x) { return num_round(x, 0); }

Value lumyr_ceil(Value x)  { return num_round(x, 1); }

// abs：绝对值（保持原类型）

Value lumyr_abs(Value x)
{
    if(x.type == VAL_DOUBLE) return lumyr_make_double(fabs(x.v.d));
    if(x.type == VAL_FLOAT) return lumyr_make_double((double)fabsf(x.v.f));  // float转换为double返回
    if(x.type == VAL_LONG_DOUBLE) return lumyr_make_long_double(fabsl(x.v.ld));
    if(is_numeric_type(x.type)) {
        /* 所有整数类型、字符类型、布尔类型：提取整数值，取绝对值，保持原类型 */
        long long val = lumyr_extract_ll(x);
        long long abs_val = val < 0 ? -val : val;
        /* 根据原类型返回对应类型 */
        switch(x.type) {
            case VAL_INT:     return lumyr_make_int(abs_val);
            case VAL_INT8:    return lumyr_make_int8((int8_t)abs_val);
            case VAL_INT16:   return lumyr_make_int16((int16_t)abs_val);
            case VAL_INT32:   return lumyr_make_int32((int32_t)abs_val);
            case VAL_INT64:   return lumyr_make_int64(abs_val);
            case VAL_BYTE:    return lumyr_make_byte((uint8_t)abs_val);
            case VAL_UINT8:   return lumyr_make_uint8((uint8_t)abs_val);
            case VAL_UINT16:  return lumyr_make_uint16((uint16_t)abs_val);
            case VAL_UINT32:  return lumyr_make_uint32((uint32_t)abs_val);
            case VAL_UINT64:  return lumyr_make_uint64((uint64_t)abs_val);
            case VAL_LONG:    return lumyr_make_long((long)abs_val);
            case VAL_ULONG:   return lumyr_make_ulong((unsigned long)abs_val);
            case VAL_SIZE_T:  return lumyr_make_size_t((size_t)abs_val);
            case VAL_SSIZE_T: return lumyr_make_ssize_t((ssize_t)abs_val);
            case VAL_BOOL:    return lumyr_make_bool(abs_val ? 1 : 0);
            case VAL_CHAR:    return lumyr_make_char((char)abs_val);
            default:          break;
        }
    }
    runtime_error("abs() 参数必须是数字");
    return val_none();
}

// sqrt：平方根（返回 double）

Value lumyr_sqrt(Value x)
{
    if(!is_numeric_type(x.type))
        runtime_error("sqrt() 参数必须是数字");
    double d = value_as_number(x);
    if(d < 0) runtime_error("sqrt() 不能对负数开方");
    return lumyr_make_double(sqrt(d));
}

// max/min：变参极值（复用比较语义：数字/字符串混合均可）

static Value extremum(Value* args, int n, int want_max)
{
    if(n < 1) runtime_error("需要至少 1 个参数");
    Value best = args[0];
    for(int i = 1; i < n; i++) {
        Value c = want_max ? lumyr_gt(args[i], best) : lumyr_lt(args[i], best);
        if(c.v.b) best = args[i];
    }
    return best;
}

Value lumyr_max(Value* args, int n) { return extremum(args, n, 1); }

Value lumyr_min(Value* args, int n) { return extremum(args, n, 0); }

/* ===== 三角/反三角/对数/指数 ===== */

/* 辅助：提取参数为 double，非数值则报错 */
static double to_double_arg(Value x, const char* fname) {
    if(!is_numeric_type(x.type)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "%s() 参数必须是数字", fname);
        runtime_error(buf);
    }
    return value_as_number(x);
}

Value lumyr_sin(Value x)  { return lumyr_make_double(sin(to_double_arg(x, "sin"))); }
Value lumyr_cos(Value x)  { return lumyr_make_double(cos(to_double_arg(x, "cos"))); }
Value lumyr_tan(Value x)  { return lumyr_make_double(tan(to_double_arg(x, "tan"))); }
Value lumyr_asin(Value x) { return lumyr_make_double(asin(to_double_arg(x, "asin"))); }
Value lumyr_acos(Value x) { return lumyr_make_double(acos(to_double_arg(x, "acos"))); }
Value lumyr_atan(Value x) { return lumyr_make_double(atan(to_double_arg(x, "atan"))); }

Value lumyr_atan2(Value y, Value x) {
    double dy = to_double_arg(y, "atan2");
    double dx = to_double_arg(x, "atan2");
    return lumyr_make_double(atan2(dy, dx));
}

Value lumyr_ln(Value x)    { return lumyr_make_double(log(to_double_arg(x, "log"))); }
Value lumyr_log10(Value x) { return lumyr_make_double(log10(to_double_arg(x, "log10"))); }
Value lumyr_log2(Value x)  { return lumyr_make_double(log2(to_double_arg(x, "log2"))); }
Value lumyr_exp(Value x)   { return lumyr_make_double(exp(to_double_arg(x, "exp"))); }

Value lumyr_pow(Value x, Value y) {
    double dx = to_double_arg(x, "pow");
    double dy = to_double_arg(y, "pow");
    return lumyr_make_double(pow(dx, dy));
}

Value lumyr_round(Value x) { return lumyr_make_double(round(to_double_arg(x, "round"))); }
Value lumyr_cbrt(Value x)  { return lumyr_make_double(cbrt(to_double_arg(x, "cbrt"))); }

Value lumyr_hypot(Value x, Value y) {
    double dx = to_double_arg(x, "hypot");
    double dy = to_double_arg(y, "hypot");
    return lumyr_make_double(hypot(dx, dy));
}

Value lumyr_sign(Value x) {
    double d = to_double_arg(x, "sign");
    if(d > 0) return lumyr_make_double(1.0);
    if(d < 0) return lumyr_make_double(-1.0);
    return lumyr_make_double(0.0);
}

Value lumyr_degrees(Value x) { return lumyr_make_double(to_double_arg(x, "degrees") * (180.0 / M_PI)); }
Value lumyr_radians(Value x) { return lumyr_make_double(to_double_arg(x, "radians") * (M_PI / 180.0)); }
Value lumyr_trunc(Value x)   { return lumyr_make_double(trunc(to_double_arg(x, "trunc"))); }

/* ============================================================
 * 每线程快速随机数发生器
 *
 * 多 worker 线程模型下，进程级 srand/rand 存在三类问题：懒播种竞争、
 * 各线程随机流相互交叠、系统 rand 内部锁串行化。这里使用 per-thread
 * 的 128 位状态移位迭代器（周期 2^128-2，通过标准统计检验套件），
 * 每线程独立状态：无锁、无共享、序列不交叠。
 *
 * 状态全零表示尚未播种，首次取数时惰性播种。播种熵源完全自包含：
 * 单调时钟纳秒 ^ 线程本地状态地址（每线程天然不同）^ 线程身份，
 * 再经单字混合扩散为两个状态字。
 * ============================================================ */
static _Thread_local uint64_t g_lmRngState[2] = {0, 0};

/* 单字混合扩散：seed 每调用一次自增一个常数，连续两次调用得到
 * 充分独立的状态字 */
static uint64_t lmRngMix(uint64_t* seed) {
    uint64_t z = (*seed += UINT64_C(0x9E3779B97F4A7C15));
    z = (z ^ (z >> 30)) * UINT64_C(0xBF58476D1CE4E5B9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94D049BB133111EB);
    return z ^ (z >> 31);
}

static void lmRngSeed(void) {
    uint64_t seed = lm_now_ns();
    seed ^= (uint64_t)(uintptr_t)&g_lmRngState;
    seed ^= (uint64_t)(uintptr_t)pthread_self();
    g_lmRngState[0] = lmRngMix(&seed);
    g_lmRngState[1] = lmRngMix(&seed);
    /* 混合结果恰为双零（概率 2^-128）时强制非零，避免退化为不动点 */
    if(g_lmRngState[0] == 0 && g_lmRngState[1] == 0)
        g_lmRngState[0] = 1;
}

/* 128 位状态移位迭代：返回下一个 64 位随机数 */
static uint64_t lmRngNext(void) {
    uint64_t x = g_lmRngState[0];
    uint64_t const y = g_lmRngState[1];
    g_lmRngState[0] = y;
    x ^= x << 23;
    g_lmRngState[1] = x ^ y ^ (x >> 17) ^ (y >> 26);
    return g_lmRngState[1] + y;
}

/* random：返回 [0,1) 随机 double。
 * 取 64 位随机数的高 53 位映射到 [0,1)，等概率、无双精度精度浪费；
 * 每线程首次调用自动播种。 */
Value lumyr_random(void) {
    if(g_lmRngState[0] == 0 && g_lmRngState[1] == 0)
        lmRngSeed();
    uint64_t bits = lmRngNext() >> 11;   /* 高 53 位 */
    return lumyr_make_double(ldexp((double)bits, -53));
}

// join：字符串数组按分隔符拼接（非字符串元素 value_to_str 转换）
