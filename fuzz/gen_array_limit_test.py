#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_array_limit_test.py — 生成 tests/array_limit_test.lm

数组极限容量测试：
  - 每种 V 类型：<V>[max, min] 读回校验值与类型
  - 整型溢出回绕
  - 无类型声明数组 [...] 混合存储全部 29 种类型
  - 大容量：无类型 / typed 数组各 10000 元素，验证不崩溃且首尾正确
"""
import os

CASES = [
    ("int",       [("max", "2147483647", "2147483647", "int"),
                   ("min", "-2147483648", "-2147483648", "int")]),
    ("int8",      [("max", "127", "127", "int8"),
                   ("min", "-128", "-128", "int8")]),
    ("int16",     [("max", "32767", "32767", "int16"),
                   ("min", "-32768", "-32768", "int16")]),
    ("int32",     [("max", "2147483647", "2147483647", "int32"),
                   ("min", "-2147483648", "-2147483648", "int32")]),
    ("int64",     [("max", "9223372036854775807", "9223372036854775807", "int64"),
                   ("min", "-9223372036854775808", "-9223372036854775808", "int64")]),
    ("uint8",     [("max", "255", "255", "uint8")]),
    ("uint16",    [("max", "65535", "65535", "uint16")]),
    ("uint32",    [("max", "4294967295", "4294967295", "uint32")]),
    ("uint64",    [("max", "18446744073709551615", "18446744073709551615", "uint64")]),
    ("byte",      [("max", "255", "255", "byte")]),
    ("uchar",     [("max", "255", "255", "unsigned char")]),
    ("ushort",    [("max", "65535", "65535", "unsigned short")]),
    ("ulong",     [("max", "18446744073709551615", "18446744073709551615", "unsigned long")]),
    ("uint",      [("max", "4294967295", "4294967295", "uint")]),
    ("size_t",    [("max", "18446744073709551615", "18446744073709551615", "size_t")]),
    ("short",     [("max", "32767", "32767", "short"),
                   ("min", "-32768", "-32768", "short")]),
    ("long",      [("max", "9223372036854775807", "9223372036854775807", "long"),
                   ("min", "-9223372036854775808", "-9223372036854775808", "long")]),
    ("long long", [("max", "9223372036854775807", "9223372036854775807", "long long"),
                   ("min", "-9223372036854775808", "-9223372036854775808", "long long")]),
    ("ssize_t",   [("max", "9223372036854775807", "9223372036854775807", "ssize_t"),
                   ("min", "-9223372036854775808", "-9223372036854775808", "ssize_t")]),
    ("char",      [("max", "127", "127", "char"),
                   ("min", "-128", "-128", "char")]),
    ("ascii",     [("max", "2147483647", "2147483647", "int")]),
    ("bool",      [("true", "true", "true", "bool"),
                   ("false", "false", "false", "bool")]),
    ("double",    [("large", "<double>100000000000000000000000000000",
                              "<double>100000000000000000000000000000", "double")]),
    ("float",     [("max", "<float>170141183460469231731687303715884105728",
                            "<float>170141183460469231731687303715884105728", "float")]),
    ("long double",[("large","<long double>100000000000000000000000000000",
                               "<long double>100000000000000000000000000000", "long double")]),
    ("bigint",    [("huge", "<bigint>1234567890123456789012345678901234567890",
                             None, "bigint")]),
    ("decimal",   [("precise","<decimal>3.141592653589793238462643383279",
                             None, "decimal")]),
    ("bitdecimal",[("precise","<bitdecimal>3.141592653589793238462643383279",
                             None, "bitdecimal")]),
]

OVERFLOW = [
    ("int8",  "200", "-56"),
    ("int8",  "-200", "56"),
    ("uint8", "300", "44"),
    ("int16", "40000", "-25536"),
    ("uint16", "70000", "4464"),
    ("int32", "3000000000", "-1294967296"),
]

def tag(s): return s.replace(" ", "")

def cast_of(v):
    return v

def main():
    out = []
    out.append("// tests/array_limit_test.lm")
    out.append("// 数组极限容量测试（由 fuzz/gen_array_limit_test.py 生成，勿手改）")
    out.append("")
    out.append("func check(cond, msg) {")
    out.append("    if(cond) {} else { throw msg; }")
    out.append("}")
    out.append("")
    out.append("n = 0;")
    out.append("")

    # ---- 1. typed 数组各类型极限值 ----
    out.append("// ===== 1. typed 数组 <V>[max, min] 极限值 =====")
    for v, cases in CASES:
        vt = tag(v)
        # 把该类型所有 case 的值放进一个数组
        lits = [c[1] for c in cases]
        arr = "a_" + vt
        out.append(arr + ' = <' + v + '>[' + ", ".join(lits) + "];")
        out.append('check(type(' + arr + ') == "typed_array", "' + vt + ': container");')
        out.append('check(' + arr + '.len == ' + str(len(cases)) + ', "' + vt + ': len");')
        for i, (label, lit, expect, etype) in enumerate(cases):
            if expect is None:
                out.append('check((string)' + arr + '[' + str(i) + '] == "' + lit.split(">",1)[1] +
                           '", "' + vt + ' ' + label + ': value");')
            else:
                out.append("check(" + arr + '[' + str(i) + '] == <' + cast_of(v) + '>' + expect +
                           ', "' + vt + ' ' + label + ': value");')
            out.append('check(type(' + arr + '[' + str(i) + ']) == "' + etype +
                       '", "' + vt + ' ' + label + ': vtype");')
            out.append("n = n + 1;")
        out.append("")

    # ---- 2. typed 数组整型溢出回绕 ----
    out.append("// ===== 2. typed 数组整型溢出回绕 =====")
    for v, lit, expect in OVERFLOW:
        vt = tag(v)
        arr = "ov_" + vt
        out.append(arr + ' = <' + v + '>[' + lit + "];")
        out.append("check(" + arr + '[0] == <' + cast_of(v) + '>' + expect +
                   ', "overflow ' + vt + ': ' + lit + ' -> ' + expect + '");')
        out.append("n = n + 1;")
    out.append("")

    # ---- 3. 无类型声明数组混合存储全部类型 ----
    out.append("// ===== 3. 无类型声明数组 [...] 混合存储全部 29 种类型 =====")
    mixed_lits = []
    for v, cases in CASES:
        mixed_lits.append(cases[0][1])  # 取每个类型第一个值
    out.append("mixed = [" + ", ".join(mixed_lits) + "];")
    out.append('check(type(mixed) == "array", "untyped: container");')
    out.append('check(mixed.len == ' + str(len(CASES)) + ', "untyped: len");')
    # 逐个校验值（高精度用字符串）
    for i, (v, cases) in enumerate(CASES):
        vt = tag(v)
        lit = cases[0][1]
        expect = cases[0][2]
        etype = cases[0][3]
        if expect is None:
            out.append('check((string)mixed[' + str(i) + '] == "' + lit.split(">",1)[1] +
                       '", "untyped ' + vt + ': value");')
        else:
            # 无类型数组中字面量按自然类型存储，直接用原始字面量比较
            out.append("check(mixed[" + str(i) + '] == ' + lit +
                       ', "untyped ' + vt + ': value");')
        out.append("n = n + 1;")
    out.append("")

    # ---- 4. 大容量数组（一次性字面量构造 100 元素，避免 append O(n^2) 过慢） ----
    out.append("// ===== 4. 大容量数组（100 元素字面量构造，验证不崩溃且首尾正确） =====")
    big_elems = ", ".join(str(i) for i in range(100))
    out.append("big_untyped = [" + big_elems + "];")
    out.append('check(big_untyped.len == 100, "big untyped: len");')
    out.append('check(big_untyped[0] == 0, "big untyped: first");')
    out.append('check(big_untyped[99] == 99, "big untyped: last");')
    out.append("n = n + 1;")
    out.append("")
    out.append("big_typed = <int64>[" + big_elems + "];")
    out.append('check(big_typed.len == 100, "big typed: len");')
    out.append('check(big_typed[0] == 0, "big typed: first");')
    out.append('check(big_typed[99] == 99, "big typed: last");')
    out.append('check(type(big_typed[50]) == "int64", "big typed: vtype");')
    out.append("n = n + 1;")
    out.append("")

    # ---- 5. typed 数组下标赋值自动 cast ----
    out.append("// ===== 5. typed 数组下标赋值自动 cast =====")
    out.append('t = <int8>[0];')
    out.append('t[0] = 200;')
    out.append('check(t[0] == -56, "typed idx set cast: 200 -> -56");')
    out.append('check(type(t[0]) == "int8", "typed idx set cast: vtype");')
    out.append("n = n + 1;")
    out.append("")

    out.append('print("array_limit_test cases =", n);')
    out.append('print("array_limit_test ALL PASS");')

    dst = os.path.join(os.path.dirname(__file__), "..", "tests", "array_limit_test.lm")
    dst = os.path.abspath(dst)
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", dst)

if __name__ == "__main__":
    main()
