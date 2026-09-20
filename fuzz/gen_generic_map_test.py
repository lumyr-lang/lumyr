#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_generic_map_test.py — 生成 tests/generic_map_test.lm

泛型 map 字面量 <K,V>{k1:v1,...} 的 K × V 全数据类型矩阵测试：
  - 键 K 覆盖所有可哈希类型（string + 数值族）
  - 值 V 覆盖所有数据类型（含高精度 bigint/decimal/bitdecimal）
每个组合断言：容器类型为 map、读回值相等、读回值类型为 V 的运行时类型名。
失败即 throw（未捕获错误使进程非 0 退出，回归可按 exit code 判定）。

用法：python3 fuzz/gen_generic_map_test.py
"""
import os

# 键类型（可哈希：string + 数值族）
KEY_TYPES = [
    "string", "int", "bool", "ascii", "char", "byte",
    "int8", "int16", "int32", "int64",
    "uint8", "uint16", "uint32", "uint", "uint64",
    "long", "long long", "ulong", "uchar",
    "short", "ushort", "size_t", "ssize_t",
]

# 值类型（全部数据类型）
VAL_TYPES = [
    "int", "double", "string", "bool", "ascii", "char", "byte",
    "int8", "int16", "int32", "int64",
    "uint8", "uint16", "uint32", "uint", "uint64",
    "long", "long long", "ulong", "uchar",
    "short", "ushort", "size_t", "ssize_t",
    "float", "long double",
    "bigint", "decimal", "bitdecimal",
]

def key_src(k):
    """entry 键源表达式 & 查找表达式（二者同形）"""
    if k == "string": return '"k"'
    if k == "char":   return "'E'"      # 69
    if k == "bool":   return "true"
    return "5"

def val_src(v):
    """entry 值源表达式"""
    if v in ("bigint", "decimal", "bitdecimal"):
        return "99"
    if v == "string":
        return '"v7"'
    if v == "char":
        return "'G'"
    if v == "bool":
        return "true"
    if v in ("double", "float", "long double"):
        return "3.5"
    return "7"

def val_expect(v):
    """读回值比较表达式（右值）"""
    if v in ("bigint", "decimal", "bitdecimal"):
        return "<" + v + ">99"
    return val_src(v)

def val_type_expect(v):
    """读回值的运行时类型名（box 后）"""
    if v in ("float", "long double"):
        return "double"      # 统一 DOUBLE 栈 + BOX_DOUBLE
    if v == "ascii":
        return "int"         # ascii 运行时即 VAL_INT
    if v == "uint":
        return "uint64"      # TOK_UINT 标注等价 CAST_UINT64
    if v == "ulong":
        return "unsigned long"
    if v == "uchar":
        return "unsigned char"
    if v == "ushort":
        return "unsigned short"
    return v

def tag(s):
    return s.replace(" ", "")

def main():
    out = []
    out.append("// tests/generic_map_test.lm")
    out.append("// 泛型 map 字面量 <K,V>{...} 全类型矩阵（由 fuzz/gen_generic_map_test.py 生成，勿手改）")
    out.append("// K=" + str(len(KEY_TYPES)) + " 可哈希类型 × V=" + str(len(VAL_TYPES)) +
               " 数据类型 = " + str(len(KEY_TYPES) * len(VAL_TYPES)) + " 组合")
    out.append("")
    out.append("// check：cond 为假则 throw（未捕获错误 → 进程非 0 退出）。")
    out.append("// 注意：动态值的逻辑非 ! 当前语义不一致，统一用 if/else 形式。")
    out.append("func check(cond, msg) {")
    out.append("    if(cond) {} else {")
    out.append("        throw msg;")
    out.append("    }")
    out.append("}")
    out.append("")
    out.append("n = 0;")
    out.append("")

    for k in KEY_TYPES:
        out.append("// ===== K = " + k + " =====")
        for v in VAL_TYPES:
            ks = key_src(k)
            m = "m_" + tag(k) + "_" + tag(v)
            label = tag(k) + "|" + tag(v)
            out.append(m + " = <" + k + ", " + v + ">{" + ks + ": " + val_src(v) + "};")
            out.append('check(type(' + m + ') == "map", "' + label + ': container");')
            if v in ("bigint", "decimal", "bitdecimal"):
                # 高精度值相等：其专用算术分支尚未实现 == 指令，经字符串内容比较
                out.append('check((string)' + m + "[" + ks + '] == "99", "' + label + ': value");')
            else:
                out.append("check(" + m + "[" + ks + "] == " + val_expect(v) + ', "' + label + ': value");')
            out.append('check(type(' + m + "[" + ks + ']) == "' + val_type_expect(v) +
                       '", "' + label + ': vtype");')
            out.append("n = n + 1;")
        out.append("")

    # 附加场景
    out.append("// ===== 附加场景 =====")
    out.append('e0 = <int, string>{};')
    out.append('check(type(e0) == "map", "empty: container");')
    out.append('check(e0[1] == null, "empty: missing key");')
    out.append("")
    out.append('e1 = <string, int>{"a": 1, "b": 2};')
    out.append('check(e1["a"] == 1 && e1["b"] == 2, "multi: values");')
    out.append('check(e1.has("a") && e1.len == 2, "multi: has/len");')
    out.append('e1["c"] = 3;')
    out.append('check(e1["c"] == 3 && e1.len == 3, "write: inserted");')
    out.append("")
    out.append('e2 = <uint, string>{2: "11", 3: "ssss"};')
    out.append('check(e2[2] == "11" && e2[3] == "ssss", "uint-key: lookup");')
    out.append("")
    out.append('print("generic_map_test cases =", n);')
    out.append('print("generic_map_test ALL PASS");')

    dst = os.path.join(os.path.dirname(__file__), "..", "tests", "generic_map_test.lm")
    dst = os.path.abspath(dst)
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", dst, len(KEY_TYPES) * len(VAL_TYPES), "cases,", len(out), "lines")

if __name__ == "__main__":
    main()
