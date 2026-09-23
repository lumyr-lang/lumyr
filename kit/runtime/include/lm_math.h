// lm_math.h —— 数学内置函数
#ifndef LM_MATH_H
#define LM_MATH_H

#include "lm_value.h"

Value lumyr_floor(Value x);  // floor(x)：向下取整，返回 int
Value lumyr_ceil(Value x);   // ceil(x)：向上取整，返回 int
Value lumyr_abs(Value x);    // abs(x)：绝对值（int/double）
Value lumyr_sqrt(Value x);   // sqrt(x)：平方根（double），负数报错
Value lumyr_max(Value* args, int n);  // max(a, b, ...)：变参最大值
Value lumyr_min(Value* args, int n);  // min(a, b, ...)：变参最小值

/* ===== 三角/反三角/对数/指数（参数取数值，返回 double） ===== */
Value lumyr_sin(Value x);     // sin(x)
Value lumyr_cos(Value x);     // cos(x)
Value lumyr_tan(Value x);     // tan(x)
Value lumyr_asin(Value x);    // asin(x)
Value lumyr_acos(Value x);    // acos(x)
Value lumyr_atan(Value x);    // atan(x)
Value lumyr_atan2(Value y, Value x);  // atan2(y, x)
Value lumyr_ln(Value x);      // 自然对数（log 别名，避免与 lumyr_log 日志冲突）
Value lumyr_log10(Value x);   // 常用对数
Value lumyr_log2(Value x);    // 二进对数
Value lumyr_exp(Value x);     // e^x
Value lumyr_pow(Value x, Value y);  // x^y
Value lumyr_round(Value x);  // 四舍五入（返回 double）
Value lumyr_cbrt(Value x);    // 立方根
Value lumyr_hypot(Value x, Value y);  // sqrt(x²+y²)
Value lumyr_sign(Value x);    // 符号 -1/0/1
Value lumyr_degrees(Value x); // 弧度→角度
Value lumyr_radians(Value x); // 角度→弧度
Value lumyr_trunc(Value x);   // 向零取整（返回 double）
Value lumyr_random(void);     // 返回 [0,1) 随机 double

#endif //LM_MATH_H
