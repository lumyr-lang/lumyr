// lm_math.c —— 数学内置函数
#include "lm_math.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>



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

/* random：返回 [0,1) 随机 double，首次调用自动播种 */
Value lumyr_random(void) {
    static int seeded = 0;
    if(!seeded) { srand((unsigned)time(NULL)); seeded = 1; }
    return lumyr_make_double((double)rand() / ((double)RAND_MAX + 1.0));
}

// join：字符串数组按分隔符拼接（非字符串元素 value_to_str 转换）
