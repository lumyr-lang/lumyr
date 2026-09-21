#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
gen_nested_types_test.py — 生成 tests/nested_types_test.lm

全数据类型（29 种）× 容器（type / map / array / class / struct）相互嵌套测试：
  A. 全类型作 typed map 的 V：<string,T>{"k": v}
  B. 全类型作 typed array 的元素：<T>[v, v]
  C. typed map 的 V = map / array / struct / type形状 / class 实例
  D. struct / class 实例作 map 的键（[expr]: 语法，引用身份语义）
  E. struct / class / type 字段含 map / array / 自定义类型（含就地修改）
  F. 深度混合链（struct->map->array->map->class 等 3 条链）
  G. 数组装容器 / 实例（动态数组 + <Pt>[...] 泛型数组）
  H. 语义保证：引用共享、空容器嵌套、多层互嵌、typed 容器嵌套就地修改
"""
import os

# (类型名, 字面量, 值比较表达式模板("{v}"替换), map中vtype, array中vtype, 高精度?)
TYPES = [
    ("int",         "7",          "{v} == 7",                 "int",            "int",            False),
    ("int8",        "7",          "{v} == 7",                 "int8",           "int8",           False),
    ("int16",       "7",          "{v} == 7",                 "int16",          "int16",          False),
    ("int32",       "7",          "{v} == 7",                 "int32",          "int32",          False),
    ("int64",       "7000000000", "{v} == 7000000000",        "int64",          "int64",          False),
    ("uint8",       "7",          "{v} == 7",                 "uint8",          "uint8",          False),
    ("uint16",      "7",          "{v} == 7",                 "uint16",         "uint16",         False),
    ("uint32",      "7",          "{v} == 7",                 "uint32",         "uint32",         False),
    ("uint64",      "7000000000", "{v} == 7000000000",        "uint64",         "uint64",         False),
    ("byte",        "7",          "{v} == 7",                 "byte",           "byte",           False),
    ("uchar",       "7",          "{v} == 7",                 "unsigned char",  "unsigned char",  False),
    ("ushort",      "7",          "{v} == 7",                 "unsigned short", "unsigned short", False),
    ("ulong",       "7000000000", "{v} == 7000000000",        "unsigned long",  "unsigned long",  False),
    ("uint",        "7000000000", "{v} == 7000000000",        "uint64",         "uint",           False),
    ("size_t",      "7000000000", "{v} == 7000000000",        "size_t",         "size_t",         False),
    ("short",       "7",          "{v} == 7",                 "short",          "short",          False),
    ("long",        "7000000000", "{v} == 7000000000",        "long",           "long",           False),
    ("long long",   "7000000000", "{v} == 7000000000",        "long long",      "long long",      False),
    ("ssize_t",     "7",          "{v} == 7",                 "ssize_t",        "ssize_t",        False),
    ("char",        "'G'",        "{v} == 'G'",               "char",           "char",           False),
    ("ascii",       "7",          "{v} == 7",                 "int",            "int",            False),
    ("bool",        "true",       "{v} == true",              "bool",           "bool",           False),
    ("double",      "3.5",        "{v} == 3.5",               "double",         "double",         False),
    ("float",       "3.5",        "{v} == 3.5",               "double",         "float",          False),
    ("long double", "3.5",        "{v} == 3.5",               "double",         "long double",    False),
    ("bigint",      "<bigint>99",     '(string){v} == "99"',  "bigint",         "bigint",         True),
    ("decimal",     "<decimal>99",    '(string){v} == "99"',  "decimal",        "decimal",        True),
    ("bitdecimal",  "<bitdecimal>99", '(string){v} == "99"',  "bitdecimal",     "bitdecimal",     True),
    ("string",      '"v7"',       '{v} == "v7"',              "string",         "string",         False),
]


# typed array 构造语义与 map cast 不同处的字面量/比较覆盖
# uint 构造为 32 位（上限 4294967295），7e9 会回绕
ARR_OVERRIDE = {
    "uint": ("4000000000", "{v} == 4000000000"),
}


def tag(t):
    return t.replace(" ", "")


def main():
    o = []
    o.append("// tests/nested_types_test.lm")
    o.append("// 全数据类型 × 容器相互嵌套测试（由 fuzz/gen_nested_types_test.py 生成，勿手改）")
    o.append("")
    o.append("func check(cond, msg) {")
    o.append("    if(cond) {} else { throw msg; }")
    o.append("}")
    o.append("")

    # ---- 自定义类型三种声明：struct / type形状 / class ----
    o.append("// ===== 0. 自定义类型：struct Pt / type BoxT / class Kls =====")
    o.append("struct Pt {")
    o.append("    x: int;")
    o.append("    y: int;")
    o.append("    func sum() {")
    o.append("        return self.x + self.y;")
    o.append("    }")
    o.append("}")
    o.append("")
    o.append("type BoxT {")
    o.append("    name: string,")
    o.append("    v: int")
    o.append("}")
    o.append("")
    o.append("class Kls {")
    o.append("    label: string;")
    o.append("    n: int;")
    o.append("    func __init__(label, n) {")
    o.append("        self.label = label;")
    o.append("        self.n = n;")
    o.append("    }")
    o.append("    func getn() {")
    o.append("        return self.n;")
    o.append("    }")
    o.append("}")
    o.append("")

    # ---- A. 全类型作 typed map 的 V ----
    o.append("// ===== A. 全 29 类型作 typed map 的 V：<string,T> =====")
    for t, lit, cmpv, mvtype, _, _ in TYPES:
        tg = tag(t)
        m = "mA_" + tg
        o.append(m + ' = <string, ' + t + '>{"k": ' + lit + "};")
        o.append('check(type(' + m + ') == "map", "A.' + tg + ': container");')
        o.append('check(' + cmpv.format(v=m + '["k"]') + ', "A.' + tg + ': value");')
        o.append('check(type(' + m + '["k"]) == "' + mvtype + '", "A.' + tg + ': vtype");')
    o.append("")

    # ---- B. 全类型作 typed array 的元素 ----
    o.append("// ===== B. 全 29 类型作 typed array 元素：<T>[v, v] =====")
    for t, lit, cmpv, _, avtype, _ in TYPES:
        tg = tag(t)
        a = "aB_" + tg
        alit, acmp = ARR_OVERRIDE.get(t, (lit, cmpv))
        o.append(a + " = <" + t + ">[" + alit + ", " + alit + "];")
        o.append('check(type(' + a + ') == "typed_array", "B.' + tg + ': container");')
        o.append('check(' + a + '.len == 2, "B.' + tg + ': len");')
        o.append('check(' + acmp.format(v=a + "[0]") + ', "B.' + tg + ': value");')
        o.append('check(type(' + a + "[0]) == \"" + avtype + '\", "B.' + tg + ': vtype");')
    o.append("")

    # ---- C. typed map V = map / array / struct / type形状 / class ----
    o.append("// ===== C. typed map 的 V = 容器 / 实例 =====")
    o.append('c_mm = <string, map>{"a": {"x": 5}};')
    o.append('check(type(c_mm) == "map", "C.mm container");')
    o.append('check(c_mm["a"]["x"] == 5, "C.mm value");')
    o.append('')
    o.append('c_ma = <string, array>{"a": [1, 2, 3]};')
    o.append('check(c_ma["a"][1] == 2, "C.ma value");')
    o.append('check(type(c_ma["a"]) == "array", "C.ma vtype");')
    o.append('')
    o.append('c_ms = <string, Pt>{"p": Pt(3, 4)};')
    o.append('check(type(c_ms["p"]) == "Pt", "C.ms vtype");')
    o.append('check(c_ms["p"].x == 3, "C.ms field");')
    o.append('check(c_ms["p"].sum() == 7, "C.ms method");')
    o.append('')
    o.append('c_mt = <string, BoxT>{"b": <BoxT>{name: "bb", v: 9}};')
    o.append('check(type(c_mt["b"]) == "map", "C.mt vtype");')
    o.append('check(c_mt["b"].name == "bb", "C.mt field");')
    o.append('check(c_mt["b"].v == 9, "C.mt field2");')
    o.append('')
    o.append('c_mc = <string, Kls>{"k": Kls("kk", 13)};')
    o.append('check(type(c_mc["k"]) == "Kls", "C.mc vtype");')
    o.append('check(c_mc["k"].label == "kk", "C.mc field");')
    o.append('check(c_mc["k"].getn() == 13, "C.mc method");')
    o.append("")

    # ---- D. 实例作 map 键 ----
    o.append("// ===== D. struct / class 实例作 map 键（引用身份） =====")
    o.append("d_pkey = Pt(1, 2);")
    o.append("d_mk = <Pt, int>{[d_pkey]: 77};")
    o.append('check(d_mk[d_pkey] == 77, "D.struct key");')
    o.append('check(d_mk[Pt(1, 2)] == null, "D.identity not equal");')
    o.append("")
    o.append('d_ckey = Kls("c", 1);')
    o.append("d_mkc = <Kls, int>{[d_ckey]: 88};")
    o.append('check(d_mkc[d_ckey] == 88, "D.class key");')
    o.append("")

    # ---- E. struct / class / type 字段含容器与自定义类型 ----
    o.append("// ===== E. 字段嵌套容器 / 实例 =====")
    o.append("class EHolder {")
    o.append("    items: array;")
    o.append("    info: map;")
    o.append("    func __init__() {")
    o.append("        self.items = [10, 20, 30];")
    o.append("        self.info = {\"a\": 1};")
    o.append("    }")
    o.append("    func total() {")
    o.append("        return self.items[0] + self.items[1] + self.items[2];")
    o.append("    }")
    o.append("}")
    o.append("eh = EHolder();")
    o.append('check(type(eh.items) == "array", "E.class array field");')
    o.append('check(eh.total() == 60, "E.class method");')
    o.append('check(eh.info["a"] == 1, "E.class map field");')
    o.append("eh.items[1] = 200;")
    o.append('check(eh.total() == 240, "E.class in-place modify");')
    o.append("eh.info[\"a\"] = 100;")
    o.append('check(eh.info["a"] == 100, "E.class map in-place");')
    o.append("")
    o.append("struct EComposed {")
    o.append("    arr: array;")
    o.append("    mp: map;")
    o.append("    p: Pt;")
    o.append("}")
    o.append("ec = <EComposed>{arr: [1, 2], mp: {\"k\": \"v\"}, p: Pt(5, 6)};")
    o.append('check(type(ec.arr) == "array", "E.struct array field");')
    o.append('check(ec.arr[0] == 1, "E.struct arr value");')
    o.append('check(ec.mp["k"] == "v", "E.struct map value");')
    o.append('check(ec.p.sum() == 11, "E.struct nested method");')
    o.append("ec.arr[0] = 11;")
    o.append('check(ec.arr[0] == 11, "E.struct in-place modify");')
    o.append("")
    o.append("type EHolderT {")
    o.append("    name: string,")
    o.append("    items: array,")
    o.append("    info: map,")
    o.append("    p: Pt")
    o.append("}")
    o.append("et = <EHolderT>{name: \"hb\", items: [4, 5, 6], info: {\"z\": 1}, p: Pt(2, 3)};")
    o.append('check(et.name == "hb", "E.type scalar field");')
    o.append('check(et.items[2] == 6, "E.type array field");')
    o.append('check(et.info["z"] == 1, "E.type map field");')
    o.append('check(et.p.sum() == 5, "E.type nested method");')
    o.append("et.items[0] = 40;")
    o.append('check(et.items[0] == 40, "E.type in-place modify");')
    o.append("")

    # ---- F. 深度混合链 ----
    o.append("// ===== F. 深度混合链 =====")
    o.append("struct FDeep {")
    o.append("    layers: map;")
    o.append("}")
    o.append("fd = <FDeep>{layers: {\"L1\": [{\"k\": Kls(\"deep\", 42)}]}};")
    o.append('check(fd.layers["L1"][0]["k"].getn() == 42, "F.chain1 method");')
    o.append("fd.layers[\"L1\"][0][\"k\"].n = 52;")
    o.append('check(fd.layers["L1"][0]["k"].getn() == 52, "F.chain1 modify");')
    o.append("")
    o.append("class FTree {")
    o.append("    children: array;")
    o.append("    node: Pt;")
    o.append("    func __init__() {")
    o.append("        self.children = [Pt(1, 1), Pt(2, 2)];")
    o.append("        self.node = Pt(9, 9);")
    o.append("    }")
    o.append("    func childSum(i) {")
    o.append("        return self.children[i].sum();")
    o.append("    }")
    o.append("}")
    o.append("ft = FTree();")
    o.append('check(ft.childSum(0) == 2, "F.chain2 method");')
    o.append('check(ft.node.sum() == 18, "F.chain2 node method");')
    o.append("ft.children[1] = Pt(10, 10);")
    o.append('check(ft.childSum(1) == 20, "F.chain2 element replace");')
    o.append("")
    o.append('fmix = {"a": [Pt(1, 2), {"q": 3}], "b": {"c": [Kls("z", 5)]}};')
    o.append('check(fmix["a"][0].sum() == 3, "F.chain3 struct in array");')
    o.append('check(fmix["a"][1]["q"] == 3, "F.chain3 map in array");')
    o.append('check(fmix["b"]["c"][0].getn() == 5, "F.chain3 class deep");')
    o.append("fmix[\"b\"][\"c\"][0].n = 6;")
    o.append('check(fmix["b"]["c"][0].getn() == 6, "F.chain3 deep modify");')
    o.append("")

    # ---- G. 数组装容器 / 实例 ----
    o.append("// ===== G. 数组装容器 / 实例 =====")
    o.append('g_coll = [{"a": 1}, [10, 20], Pt(7, 8), Kls("g", 3)];')
    o.append('check(type(g_coll) == "array", "G.coll container");')
    o.append('check(g_coll[0]["a"] == 1, "G.map element");')
    o.append('check(g_coll[1][0] == 10, "G.array element");')
    o.append('check(g_coll[2].sum() == 15, "G.struct element method");')
    o.append('check(g_coll[3].getn() == 3, "G.class element method");')
    o.append("")
    o.append("// 自定义类型泛型数组：<Pt>[1, 2] → [Pt(1), Pt(2)] 形状构造，普通数组")
    o.append("g_shaped = <Pt>[1, 2];")
    o.append('check(type(g_shaped) == "array", "G.shape container");')
    o.append('check(g_shaped.len == 2, "G.shape len");')
    o.append('check(type(g_shaped[0]) == "Pt", "G.shape elem vtype");')
    o.append('check(g_shaped[0].x == 1, "G.shape elem0 x");')
    o.append('check(g_shaped[0].sum() == 1, "G.shape elem0 method");')
    o.append('check(g_shaped[1].x == 2, "G.shape elem1 x");')
    o.append("g_shaped[1] = Pt(20, 20);")
    o.append('check(g_shaped[1].sum() == 40, "G.shape element replace");')
    o.append("")

    # ---- H. 语义保证 ----
    o.append("// ===== H. 语义：引用共享 / 空容器嵌套 / 多层互嵌 =====")
    o.append("h_shared = {\"n\": 1};")
    o.append("h_outer1 = {\"s\": h_shared};")
    o.append("h_outer2 = {\"s\": h_shared};")
    o.append("h_outer1[\"s\"][\"n\"] = 99;")
    o.append('check(h_outer2["s"]["n"] == 99, "H.reference sharing");')
    o.append("")
    o.append('h_empty = {"a": [], "b": {}};')
    o.append('check(h_empty["a"].len == 0, "H.empty array");')
    o.append('h_empty["a"].add(1);')
    o.append('check(h_empty["a"][0] == 1, "H.empty array grow");')
    o.append('h_empty["b"]["k"] = 2;')
    o.append('check(h_empty["b"]["k"] == 2, "H.empty map grow");')
    o.append("")
    o.append("h_m3 = [[[1]]];")
    o.append('check(type(h_m3[0][0]) == "array", "H.nested array type");')
    o.append('check(h_m3[0][0][0] == 1, "H.nested array value");')
    o.append("")
    o.append('h_tmm = <string, map>{"a": {"x": 1}};')
    o.append('h_tmm["a"]["x"] = 11;')
    o.append('check(h_tmm["a"]["x"] == 11, "H.typed map V=map modify");')
    o.append('h_tma = <string, array>{"a": [1, 2]};')
    o.append('h_tma["a"][0] = 9;')
    o.append('check(h_tma["a"][0] == 9, "H.typed map V=array modify");')
    o.append("")
    o.append('print("nested_types_test ALL PASS");')

    dst = os.path.join(os.path.dirname(__file__), "..", "tests", "nested_types_test.lm")
    with open(dst, "w", encoding="utf-8") as f:
        f.write("\n".join(o) + "\n")
    print("wrote", dst)


if __name__ == "__main__":
    main()
