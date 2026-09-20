#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_map_limit_test.py — 生成 tests/map_limit_test.lm

按每个数据类型的最大/最小容量测试 map：
  - 每种 V 类型：在 <string, V> 中存入 max（及 signed 的 min），读回校验值与类型
  - 整型族额外校验溢出回绕（max+1 截断到类型范围）
  - 高精度 bigint/decimal/bitdecimal 用超大/高精度值校验
"""
import os

# (类型名, [(标签, 字面量, 期望值或None, 期望类型名)])
# 期望值 None 表示用 (string)x == "字面量" 比较（高精度）
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
    ("uint",      [("max", "18446744073709551615", "18446744073709551615", "uint64")]),
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
    ("float",     [("max", "<float>340000000000000000000000000000000000000",
                            "<float>340000000000000000000000000000000000000", "double")]),
    ("long double",[("large","<long double>100000000000000000000000000000",
                               "<long double>100000000000000000000000000000", "double")]),
    ("bigint",    [("huge", "<bigint>1234567890123456789012345678901234567890",
                             None, "bigint")]),
    ("decimal",   [("precise","<decimal>3.141592653589793238462643383279",
                             None, "decimal")]),
    ("bitdecimal",[("precise","<bitdecimal>3.141592653589793238462643383279",
                             None, "bitdecimal")]),
]

# 整型溢出回绕：(类型, 超容量字面量, 期望值)
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
    """比较用期望值的 cast 类型名（需与 map 的 <string,V> 实际 cast 一致）"""
    return "uint64" if v == "uint" else v

def main():
    out = []
    out.append("// tests/map_limit_test.lm")
    out.append("// map 各数据类型极限容量测试（由 fuzz/gen_map_limit_test.py 生成，勿手改）")
    out.append("")
    out.append("func check(cond, msg) {")
    out.append("    if(cond) {} else { throw msg; }")
    out.append("}")
    out.append("")
    out.append("n = 0;")
    out.append("")

    for v, cases in CASES:
        vt = tag(v)
        out.append("// ===== V = " + v + " =====")
        for label, lit, expect, etype in cases:
            m = "m_" + vt + "_" + label
            out.append(m + ' = <string, ' + v + '>{"v": ' + lit + "};")
            out.append('check(type(' + m + ') == "map", "' + vt + ' ' + label + ': container");')
            if expect is None:
                # 高精度：经字符串内容比较
                out.append('check((string)' + m + '["v"] == "' + lit.split(">",1)[1] +
                           '", "' + vt + ' ' + label + ': value");')
            else:
                # 数值期望值用 <V> 包裹：负整数字面量超 int32 会被解析为 double，
                # 显式 cast 保证两侧同类型比较
                out.append("check(" + m + '["v"] == <' + cast_of(v) + '>' + expect +
                           ', "' + vt + ' ' + label + ': value");')
            out.append('check(type(' + m + '["v"]) == "' + etype +
                       '", "' + vt + ' ' + label + ': vtype");')
            out.append("n = n + 1;")
        out.append("")

    # 整型溢出回绕
    out.append("// ===== 整型溢出回绕（max+1 截断到类型范围，不应崩溃） =====")
    for v, lit, expect in OVERFLOW:
        vt = tag(v)
        m = "ov_" + vt
        out.append(m + ' = <string, ' + v + '>{"v": ' + lit + "};")
        out.append("check(" + m + '["v"] == <' + cast_of(v) + '>' + expect +
                   ', "overflow ' + vt + ': ' + lit + ' -> ' + expect + '");')
        out.append("n = n + 1;")
    out.append("")

    out.append('print("map_limit_test cases =", n);')
    out.append('print("map_limit_test ALL PASS");')

    dst = os.path.join(os.path.dirname(__file__), "..", "tests", "map_limit_test.lm")
    dst = os.path.abspath(dst)
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", dst)

if __name__ == "__main__":
    main()
