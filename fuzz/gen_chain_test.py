#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
链式混合运算测试生成器 + 独立 oracle。
从 tests/random_type_test.lm 的 1500 个案例中提取操作数（类型+字面量），
生成 a+b+c+d / a*b+c*d / (a+b)*(c-d) 等多操作数链式混合运算，
并用与 VM 运行时相同的语义规则独立计算期望的「类型+值」。

产物：
  tests/chain_mixed_test.lm   生成的测试源码（固定随机种子，可复现）
  <系统临时目录>/chain_expected.txt   每行一个期望结果 C<id>:<类型>=<值>

用法（在仓库根目录）：
  python3 fuzz/gen_chain_test.py
  T="${TMPDIR:-/tmp}"
  ./bin/lumyr tests/chain_mixed_test.lm 2>/dev/null | grep -E '^C[0-9]+:' > "$T/chain_actual.txt"
  diff "$T/chain_expected.txt" "$T/chain_actual.txt"   # 0 行即全部通过
"""
import re
import random
import math
import sys
import tempfile
from fractions import Fraction
from pathlib import Path

# 仓库根目录 = fuzz/ 的上一级；脚本可从任意工作目录执行
ROOT = Path(__file__).resolve().parent.parent
SRC = str(ROOT / "tests" / "random_type_test.lm")
OUT_LM = str(ROOT / "tests" / "chain_mixed_test.lm")
OUT_EXPECT = str(Path(tempfile.gettempdir()) / "chain_expected.txt")

# ---------- 类型分类（与 ir_compile.c 提升规则一致） ----------
CAT_STRING = "string"
CAT_BIGINT = "bigint"
CAT_BITDECIMAL = "bitdecimal"
CAT_DECIMAL = "decimal"
CAT_DOUBLE = "double"
CAT_INT = "int"

DOUBLE_TYPES = {"float", "double"}

def category(t):
    if t == "string":
        return CAT_STRING
    if t == "bigint":
        return CAT_BIGINT
    if t == "bitdecimal":
        return CAT_BITDECIMAL
    if t == "decimal":
        return CAT_DECIMAL
    if t in DOUBLE_TYPES:
        return CAT_DOUBLE
    return CAT_INT  # 所有整数族 + bool + char 都走 INT64 栈

# ---------- 全量类型操作数池 ----------
# 覆盖语言中全部 20+ 种数据类型，每类型多份随机值
INT_TYPES = [
    ("int", (None, None)), ("int8", (-128, 127)), ("int16", (-32768, 32767)),
    ("int32", (None, None)), ("int64", (None, None)),
    ("uint", (0, 4294967295)), ("uint8", (0, 255)),
    ("uint16", (0, 65535)), ("uint32", (0, 4294967295)),
    ("uint64", (0, 18446744073709551615)),
    ("long", (None, None)), ("long long", (None, None)),
    ("ulong", (0, 18446744073709551615)),
    ("short", (-32768, 32767)), ("ushort", (0, 65535)),
    ("byte", (0, 255)), ("uchar", (0, 255)),
    ("char", (32, 126)), ("ascii", (32, 126)),
    ("bool", (0, 1)), ("size_t", (0, 18446744073709551615)),
    ("ssize_t", (None, None)),
]
FLOAT_TYPES = ["float", "double", "long double"]
HP_TYPES = ["bigint", "decimal", "bitdecimal"]

# bigint/decimal/bitdecimal 字面量池（精挑边界值）
BIGINT_LITS = ["0", "1", "-1", "100", "-99", "999999999999999999",
               "1000000000000000000", "-99999999999999999999", "42", "-7"]
DECIMAL_LITS = ["0.0", "1.5", "-2.75", "0.1", "0.2", "3.14159",
                "-0.001", "123.456", "0.99999", "-99.9", "1.0", "0.0001"]
BD_LITS = ["1.2", "0.3", "-2.75", "5.858", "123.456", "0.000001",
           "999.999999", "-0.125", "1000000.5", "7", "42.042", "-12345.6789",
           "3.14159265", "0.5", "0.0", "-1.0"]

def gen_full_pool(rng):
    """生成覆盖全部 20+ 类型的操作数池"""
    pool = []
    # 整数族：每类型 20 个随机值
    # char/ascii 需排除 ' (39) 和 \ (92)，词法器不支持转义
    for tname, (lo, hi) in INT_TYPES:
        lo_val = lo if lo is not None else -10000
        hi_val = hi if hi is not None else 10000
        for _ in range(20):
            v = rng.randint(lo_val, hi_val)
            if tname in ("char", "ascii") and v in (39, 92):
                # 重采样避开词法器不支持的字符
                while v in (39, 92):
                    v = rng.randint(lo_val, hi_val)
            raw = str(v)
            if tname == "bool":
                raw = "true" if v else "false"
            elif tname in ("char", "ascii"):
                raw = "'%s'" % chr(v)
            pool.append((CAT_INT, tname, raw, v))
    # 浮点族：每类型 20 个
    for tname in FLOAT_TYPES:
        for _ in range(20):
            v = rng.uniform(-9999.0, 9999.0)
            # 避免 NaN/Inf
            if not math.isfinite(v):
                continue
            pool.append((CAT_DOUBLE, tname, repr(v), v))
    # 字符串：20 个
    STR_LITS = ["abc", "hello", "123", "x", "", "test", "42", "3.14",
                "a", "xyz", "0", "-5", "data", "value", "99",
                "true", "q", "42.5", "tag", "100"]
    for s in STR_LITS:
        pool.append((CAT_STRING, "string", '"%s"' % s, s))
    # bigint：每字面量 3 份
    for s in BIGINT_LITS:
        for _ in range(3):
            pool.append((CAT_BIGINT, "bigint", '"%s"' % s, int(s)))
    # decimal：每字面量 3 份
    for s in DECIMAL_LITS:
        u, p = dec_parse(s)
        for _ in range(3):
            pool.append((CAT_DECIMAL, "decimal", '"%s"' % s, (u, p)))
    # bitdecimal：每字面量 3 份
    for s in BD_LITS:
        prec = len(s.split(".")[1]) if "." in s else 0
        for _ in range(3):
            pool.append((CAT_BITDECIMAL, "bitdecimal", '"%s"' % s, (Fraction(s), prec)))
    return pool

# ---------- 提取操作数池 ----------
def load_pool():
    pool = []
    assign_re = re.compile(r'^\s*[ab]\d+\s*=\s*<(\w+)>(.+?);\s*$')
    with open(SRC, encoding="utf-8") as f:
        for line in f:
            m = assign_re.match(line)
            if not m:
                continue
            tname, raw = m.group(1), m.group(2).strip()
            cat = category(tname)
            raw_lit = raw  # 原样重新发射，保证浮点数字面量位模式一致
            content = raw_lit[1:-1] if raw_lit.startswith('"') else raw_lit
            if cat == CAT_STRING:
                val = content
            elif cat == CAT_BIGINT:
                val = int(content)
            elif cat == CAT_DECIMAL:
                val = dec_parse(content)      # (unscaled, prec)
            elif cat == CAT_DOUBLE:
                val = float(raw_lit)
            elif tname == "bool":
                val = 1 if raw_lit == "true" else 0
            elif tname == "char":
                val = ord(raw_lit)
                raw_lit = "'" + raw_lit + "'"   # char 字面量必须单引号
            else:
                val = int(raw_lit)
            pool.append((cat, tname, raw_lit, val))
    return pool

# ---------- 数值转换辅助（模拟编译器 emit 的跨栈转换 + 运行时） ----------
def c_int_to_str(i):
    return str(i)

def to_int64(v):
    """模拟 C int64_t 溢出：掩码到 64 位有符号整数"""
    v = v & 0xFFFFFFFFFFFFFFFF
    if v >= 0x8000000000000000:
        v -= 0x10000000000000000
    return v

def c_double_to_str(x):
    # vm_exec_conv_double_to_string: snprintf("%f")，默认 6 位小数
    return f"{x:.6f}"

def c_atoi(s):
    # C atoi：解析开头可选符号+数字，无法解析为 0；返回 int（32 位有符号）
    m = re.match(r'^(-?)(\d+)', s)
    if not m:
        return 0
    v = -int(m.group(2)) if m.group(1) else int(m.group(2))
    # atoi 返回 int (32 位)，模拟溢出截断
    v = v & 0xFFFFFFFF
    if v >= 0x80000000:
        v -= 0x100000000
    return v

def c_is_num_str(s):
    # vm_exec_arith_ptr_*：每个字符只能是数字/'-'/'.'
    return len(s) > 0 and all(ch.isdigit() or ch in "-." for ch in s)

def trunc_div(a, b):
    # C 整数除法：向零截断；调用处保证 b != 0
    q = abs(a) // abs(b)
    return q if (a >= 0) == (b >= 0) else -q

# ---------- decimal：模拟 lm_decimal.c 的定点 bigint 运算 ----------
# 内部表示：(unscaled:int, prec:int)
def dec_from_value(cat, v):
    if cat == CAT_INT:
        return (to_int64(v), 0)
    if cat == CAT_BIGINT:
        return (v, 0)                      # bigint_to_string 是纯整数
    if cat == CAT_DOUBLE:
        s = c_double_to_str(v)            # DOUBLE_TO_STRING 用 %f → 6 位小数
        return dec_parse(s)
    if cat == CAT_DECIMAL:
        return (v[0], v[1])               # 已是定点元组
    raise ValueError(cat)

def dec_parse(s):
    neg = s.startswith("-")
    s2 = s[1:] if neg else s
    if "." in s2:
        ip, fp = s2.split(".", 1)
        u = int(ip + fp) if (ip + fp) else 0
        return (-u if neg else u, len(fp))
    u = int(s2) if s2 else 0
    return (-u if neg else u, 0)

def dec_format(u, p):
    # 模拟 bigint_to_decimal（保留尾随 0）
    if p == 0:
        return str(u)
    neg = u < 0
    digits = str(-u if neg else u)
    if len(digits) <= p:
        body = "0." + "0" * (p - len(digits)) + digits
    else:
        body = digits[:len(digits) - p] + "." + digits[len(digits) - p:]
    return ("-" + body) if neg else body

def dec_op(a, b, op):
    ua, pa = a
    ub, pb = b
    if op in ("+", "-"):
        p = max(pa, pb)
        xa = ua * 10 ** (p - pa)
        xb = ub * 10 ** (p - pb)
        r = xa + xb if op == "+" else xa - xb
        return (r, p)
    if op == "*":
        return (ua * ub, pa + pb)
    if op == "/":
        if ub == 0:
            raise ZeroDivisionError
        result_prec = 20
        total_pad = max(0, result_prec + (pb - pa))
        num = ua * 10 ** total_pad
        q = trunc_div(num, ub)
        return (q, result_prec)
    raise ValueError(op)

# ---------- bigint 转换：from_string 只读取小数点前的数字 ----------
def bigint_from_other(cat, v):
    if cat == CAT_INT:
        return to_int64(v)
    if cat == CAT_BIGINT:
        return v
    if cat == CAT_DOUBLE:
        s = c_double_to_str(v)
        m = re.match(r'^(-?)(\d+)', s)
        return -int(m.group(2)) if m and m.group(1) else (int(m.group(2)) if m else 0)
    if cat == CAT_DECIMAL:
        s = dec_format(v[0], v[1])          # 运行时走 DECIMAL_TO_STRING（原始定点串）
        m = re.match(r'^(-?)(\d+)', s)
        return -int(m.group(2)) if m and m.group(1) else (int(m.group(2)) if m else 0)
    if cat == CAT_BITDECIMAL:
        s = bd_format(v[0], v[1])           # 运行时走 BITDECIMAL_TO_STRING（定点 p 位小数）
        m = re.match(r'^(-?)(\d+)', s)
        return -int(m.group(2)) if m and m.group(1) else (int(m.group(2)) if m else 0)
    raise ValueError(cat)

# ---------- bitdecimal：模拟 lm_bitdecimal.c 的 GMP mpf_t 运算 ----------
# 内部表示：(exact_value:Fraction, prec:int)；512 位 mpf 足以让 p<=60 输出精确
def bd_from_value(cat, v):
    if cat == CAT_INT:
        return (Fraction(to_int64(v)), 0)   # OPC_BITDECIMAL_FROM_INT64，精确
    if cat == CAT_DOUBLE:
        return (Fraction(v), 15)           # OPC_BITDECIMAL_FROM_DOUBLE，precision=15
    if cat == CAT_DECIMAL:
        return (Fraction(v[0], 10 ** v[1]), v[1])  # DECIMAL_TO_STRING → FROM_STRING
    if cat == CAT_BITDECIMAL:
        return v
    raise ValueError(cat)

def bd_op(a, b, op):
    va, pa = a
    vb, pb = b
    if op == "+":
        return (va + vb, max(pa, pb))
    if op == "-":
        return (va - vb, max(pa, pb))
    if op == "*":
        return (va * vb, pa + pb)
    if op == "/":
        if vb == 0:
            raise ZeroDivisionError
        return (va / vb, pa + 10)          # 除法多保留 10 位
    raise ValueError(op)

def bd_format(val, p):
    """按 p 位小数半偶舍入输出（复刻 BITDECIMAL_TO_STRING），无 dot 时 p=0"""
    neg = val < 0
    v = -val if neg else val
    scale = 10 ** p
    q, r = divmod(v.numerator * scale, v.denominator)
    twice = r * 2
    if twice > v.denominator or (twice == v.denominator and q % 2 == 1):
        q += 1
    digits = str(q)
    if p > 0:
        if len(digits) <= p:
            digits = "0" * (p - len(digits) + 1) + digits
        s = digits[:-p] + "." + digits[-p:]
    else:
        s = digits
    return ("-" + s) if neg else s

# ---------- 任意值转字符串（模拟字符串拼接路径） ----------
def to_string(cat, v):
    if cat == CAT_INT:
        return c_int_to_str(to_int64(v))
    if cat == CAT_DOUBLE:
        return c_double_to_str(v)
    if cat == CAT_BIGINT:
        return str(v)
    if cat == CAT_DECIMAL:
        return dec_format(*v)
    if cat == CAT_BITDECIMAL:
        return bd_format(*v)
    return v  # string

# ---------- 字符串栈四运算 ----------
def ptr_op(a, b, op):
    if op == "+":
        return a + b
    if op == "*":
        an, bn = c_is_num_str(a), c_is_num_str(b)
        if an and not bn:
            s, n = b, c_atoi(a)
        elif not an and bn:
            s, n = a, c_atoi(b)
        else:
            s, n = a, c_atoi(b)
        if n <= 0:
            return ""
        # 复刻运行时：atoi 落入 int 范围；重复上限 100 万次、256MB
        n = min(n, 2**31 - 1)
        if n > 1_000_000:
            n = 1_000_000
        if len(s) * n > 256 * 1024 * 1024:
            n = 256 * 1024 * 1024 // len(s)
        return s * n
    if op == "/":
        n = c_atoi(b)
        if n <= 0:
            return ""
        return a[:len(a) // n]
    if op == "-":
        an, bn = c_is_num_str(a), c_is_num_str(b)
        if an and not bn:
            s, n, left = b, c_atoi(a), False   # 首部截取
        elif not an and bn:
            s, n, left = a, c_atoi(b), True    # 尾部截取
        else:
            s, n, left = a, c_atoi(b), True
        if n <= 0 or n >= len(s):
            return ""
        return s[:len(s) - n] if left else s[n:]
    raise ValueError(op)

# ---------- 表达式求值（树结构，叶子=操作数序号） ----------
class SkipCase(Exception):
    pass

def promote(c1, c2):
    # 与 c_expr_cast_type 优先级一致：string > bigint > bitdecimal > decimal > double > int
    for t in (CAT_STRING, CAT_BIGINT, CAT_BITDECIMAL, CAT_DECIMAL, CAT_DOUBLE):
        if c1 == t or c2 == t:
            return t
    return CAT_INT

def eval_node(node, operands):
    """返回 (category, value)"""
    if isinstance(node, int):
        cat = operands[node][0]
        val = operands[node][3]
        if cat == CAT_INT:
            val = to_int64(val)
        return cat, val
    op, l, r = node
    cl, vl = eval_node(l, operands)
    cr, vr = eval_node(r, operands)
    res = promote(cl, cr)

    if res == CAT_STRING:
        return CAT_STRING, ptr_op(to_string(cl, vl), to_string(cr, vr), op)

    if res == CAT_BIGINT:
        bi_l = vl if cl == CAT_BIGINT else bigint_from_other(cl, vl)
        bi_r = vr if cr == CAT_BIGINT else bigint_from_other(cr, vr)
        if op == "+":
            return CAT_BIGINT, bi_l + bi_r
        if op == "-":
            return CAT_BIGINT, bi_l - bi_r
        if op == "*":
            return CAT_BIGINT, bi_l * bi_r
        if op == "/":
            if bi_r == 0:
                raise ZeroDivisionError
            return CAT_BIGINT, trunc_div(bi_l, bi_r)

    if res == CAT_BITDECIMAL:
        bl = vl if cl == CAT_BITDECIMAL else bd_from_value(cl, vl)
        br = vr if cr == CAT_BITDECIMAL else bd_from_value(cr, vr)
        return CAT_BITDECIMAL, bd_op(bl, br, op)

    if res == CAT_DECIMAL:
        dl = (vl[0], vl[1]) if cl == CAT_DECIMAL else dec_from_value(cl, vl)
        dr = (vr[0], vr[1]) if cr == CAT_DECIMAL else dec_from_value(cr, vr)
        return CAT_DECIMAL, dec_op(dl, dr, op)

    if res == CAT_DOUBLE:
        dl = float(to_int64(vl)) if cl == CAT_INT else vl
        dr = float(to_int64(vr)) if cr == CAT_INT else vr
        if op == "/":
            if dr == 0.0:
                raise ZeroDivisionError
            r = dl / dr
        else:
            r = {"+": dl + dr, "-": dl - dr, "*": dl * dr}[op]
        if not math.isfinite(r):
            raise SkipCase
        return CAT_DOUBLE, r

    # int
    if op == "/":
        if vr == 0:
            raise ZeroDivisionError
        return CAT_INT, to_int64(trunc_div(vl, vr))
    return CAT_INT, to_int64({"+": vl + vr, "-": vl - vr, "*": vl * vr}[op])

# ---------- 表达式树构造 ----------
N = lambda op, a, b: (op, a, b)

def left_chain(ops):
    """ops 长度 k → k+1 个叶子的左结合链"""
    tree = 0
    for i, op in enumerate(ops):
        tree = N(op, tree, i + 1)
    return tree

def right_chain(ops):
    """ops 长度 k → k+1 个叶子的右结合链"""
    n = len(ops)
    tree = n
    for i in range(n - 1, -1, -1):
        tree = N(ops[i], i, tree)
    return tree

SHAPES = [
    N("+", 0, N("*", 1, 2)),                       # a + b*c（优先级）
    N("+", N("*", 0, 1), N("*", 2, 3)),            # a*b + c*d
    N("*", N("+", 0, 1), N("-", 2, 3)),            # (a+b)*(c-d)
    N("-", N("+", 0, N("*", 1, 2)), N("/", 3, 4)), # a+b*c - d/e
    # 新增形状
    N("/", N("+", 0, 1), N("-", 2, 3)),            # (a+b)/(c-d)
    N("*", N("/", 0, 1), N("+", 2, 3)),            # (a/b)*(c+d)
    N("+", N("-", 0, 1), N("*", 2, 3)),            # (a-b)+c*d
    N("-", N("*", 0, 1), N("/", 2, 3)),            # a*b - c/d
    N("+", N("+", 0, 1), N("-", 2, 3)),            # (a+b)+(c-d) 同优先级
    N("*", N("*", 0, 1), N("*", 2, 3)),            # a*b*c*d（全乘）
    N("+", N("/", 0, 1), N("/", 2, 3)),            # a/b + c/d（全除）
    # 5 叶子
    N("+", N("*", 0, 1), N("+", N("*", 2, 3), 4)),  # a*b + c*d + e
    N("*", N("+", 0, 1), N("+", 2, N("+", 3, 4))),  # (a+b)*(c+d+e)
    N("-", N("+", 0, N("+", 1, 2)), N("*", 3, 4)),  # (a+b+c) - d*e
]
SHAPE_ARITY = [3, 4, 4, 5, 4, 4, 4, 4, 4, 4, 4, 5, 5, 5]

# ---------- 树 → 源码（叶子引用变量名） ----------
def emit_node(node, names):
    if isinstance(node, int):
        return names[node]
    op, l, r = node
    return "(" + emit_node(l, names) + " " + op + " " + emit_node(r, names) + ")"

def result_str(cat, v):
    return to_string(cat, v)

# ---------- 主生成流程 ----------
def main():
    rng = random.Random(20260919)
    pool = gen_full_pool(rng)
    assert len(pool) >= 500, len(pool)

    # 合并 random_type_test 的操作数（增加历史覆盖）
    old_pool = load_pool()
    pool = pool + old_pool

    rng = random.Random(20260919)

    cases = []  # (id, tree, arity, [(cat,tname,raw,val)])

    seq_templates = []
    # 左结合链（2~6 操作数）
    for s in ["++", "+-", "+*", "+/", "-+", "--", "-*", "*+", "*-", "**", "*/", "/+", "/*"]:
        seq_templates.append((left_chain(list(s)), 3))
    for s in ["+++", "++-", "+*-", "*+*", "+-+", "*/+", "**+", "++*", "-+/", "*/*", "---", "///",
              "+/*", "/*-", "*-+", "-*/", "/+-", "*/*", "+-/", "/-+"]:
        seq_templates.append((left_chain(list(s)), 4))
    for s in ["++++", "*+*+", "+-+-", "**+*", "+-++", "****", "++*/", "*-*+", "+*-*", "/++*",
              "-+-+", "*/*-", "+++*", "***/", "+/*+", "-*-/", "/+-/", "*/**", "++-/", "*+-*"]:
        seq_templates.append((left_chain(list(s)), 5))
    for s in ["+++++", "*+*++", "+-+-+"]:
        seq_templates.append((left_chain(list(s)), 6))
    for s in ["***+**", "+-++*+", "****++"]:
        seq_templates.append((left_chain(list(s)), 7))
    # 右结合链（确保结合顺序不影响结果）
    for s in ["++", "*+", "+*", "**", "*/", "/*"]:
        seq_templates.append((right_chain(list(s)), 3))
    for s in ["+++", "***", "*+*", "*/+", "+/*"]:
        seq_templates.append((right_chain(list(s)), 4))
    for tree, arity in zip(SHAPES, SHAPE_ARITY):
        seq_templates.append((tree, arity))

    # 类型对覆盖模板：4 运算 × 2 操作数，确保不同类型直接组合
    for op in ["+", "-", "*", "/"]:
        seq_templates.append((N(op, 0, 1), 2))

    # 每个模板多次随机抽样，直到拿够合法案例
    PER = 100

    for tree, arity in seq_templates:
        got = 0
        attempts = 0
        while got < PER and attempts < 1000:
            attempts += 1
            ops = tuple(rng.choice(pool) for _ in range(arity))
            try:
                cat, v = eval_node(tree, ops)
            except (ZeroDivisionError, SkipCase):
                continue
            # 输出体积/极端值护栏
            if cat == CAT_STRING and len(v) > 200:
                continue
            if cat == CAT_BIGINT and len(str(abs(v))) > 80:
                continue
            if cat == CAT_BITDECIMAL and (v[1] > 60 or len(bd_format(*v)) > 60):
                continue
            cases.append((len(cases) + 1, tree, arity, ops, cat, v))
            got += 1

    # ---------- 手工边界/语义用例（同 oracle 计算） ----------
    # (说明, 声明列表[(tname, raw)], 表达式源码, 表达式树或 None)
    edge_cases = []

    def ec(desc, decls, expr_src, tree):
        edge_cases.append((desc, decls, expr_src, tree))

    ec("整数优先级 2+3*4", [], "2 + 3 * 4", N("+", 2, N("*", 3, 4)))
    ec("括号改变优先级 (2+3)*4", [], "(2 + 3) * 4", N("*", N("+", 2, 3), 4))
    ec("整数除零得 0", [], "10 / 0", N("/", 10, 0))
    ec("浮点除零得 0", [], "3.0 / 0.0", N("/", 3.0, 0.0))
    ec("负数参与链式运算", [("int", "-5")], "v0 + 3 * 4", N("+", 0, N("*", 3, 4)))
    ec("整数向零截断 -7/2", [], "(-7) / 2", N("/", -7, 2))
    ec("decimal 0.1+0.2", [], '<decimal>"0.1" + <decimal>"0.2"',
       N("+", ("dec", "0.1"), ("dec", "0.2")))
    ec("double 0.1+0.2（%%f 6 位）", [], "0.1 + 0.2", N("+", 0.1, 0.2))
    ec("字符串链式拼接", [], '"a" + 1 + 2', N("+", N("+", "a", 1), 2))
    ec("字符串重复后拼接", [], '"ab" * 3 + "cd"', N("+", N("*", "ab", 3), "cd"))
    ec("字符串除法前缀截取", [], '"abcabc" / 3', N("/", "abcabc", 3))
    ec("字符串减法尾部截取", [], '"abcabc" - 3', N("-", "abcabc", 3))
    ec("数字减字符串首部截取", [], '3 - "abcabc"', N("-", 3, "abcabc"))
    ec("bool+char 整数化 true+65+1", [], "true + <char>A + 1",
       N("+", N("+", ("bool", 1), ("char", 65)), 1))
    ec("byte 不截断 876+1", [], "<byte>876 + 1", N("+", ("intraw", 876), 1))
    ec("bigint 大整数链", [],
       '<bigint>"1000000000000000000" * <int>2 + <int>5',
       N("+", N("*", ("bi", 10**18), 2), 5))
    ec("decimal 链式精度 1.2*0.3+0.04", [],
       '<decimal>"1.2" * <decimal>"0.3" + <decimal>"0.04"',
       N("+", N("*", ("dec", "1.2"), ("dec", "0.3")), ("dec", "0.04")))
    ec("decimal 除法保留 20 位", [], '<decimal>"10" / <decimal>"4"',
       N("/", ("dec", "10"), ("dec", "4")))
    ec("float 自动提升 double", [],
       "<float>1.5 + <int>2 + <double>3.25",
       N("+", N("+", ("dbl", 1.5), 2), ("dbl", 3.25)))
    ec("混合 (int+double)*int", [("int", "6")],
       "(v0 + <double>1.5) * <int>2", N("*", N("+", 0, ("dbl", 1.5)), 2))
    ec("bigint 吸收 int 链", [],
       '<bigint>"100" + <int>20 + <int>3',
       N("+", N("+", ("bi", 100), 20), 3))
    ec("decimal 吸收 int 链", [],
       '<decimal>"1.5" + <int>2 + <int>10',
       N("+", N("+", ("dec", "1.5"), 2), 10))
    ec("string 与 bool/char 拼接", [],
       '"x" + true + <char>A', N("+", N("+", "x", ("bool", 1)), ("char", 65)))
    ec("五连加全整数 1+2+3+4+5", [], "1 + 2 + 3 + 4 + 5",
       N("+", N("+", N("+", N("+", 1, 2), 3), 4), 5))
    # ---- bitdecimal 系列 ----
    ec("bitdecimal 基本加法 1.2+0.3", [],
       '<bitdecimal>"1.2" + <bitdecimal>"0.3"',
       N("+", ("bd", "1.2"), ("bd", "0.3")))
    ec("bitdecimal 乘法精度 1.2*0.3", [],
       '<bitdecimal>"1.2" * <bitdecimal>"0.3"',
       N("*", ("bd", "1.2"), ("bd", "0.3")))
    ec("bitdecimal 除法保留 prec+10 位 10/4", [],
       '<bitdecimal>"10" / <bitdecimal>"4"',
       N("/", ("bd", "10"), ("bd", "4")))
    ec("bitdecimal 循环小数 1/3", [],
       '<bitdecimal>"1" / <bitdecimal>"3"',
       N("/", ("bd", "1"), ("bd", "3")))
    ec("bitdecimal 吸收 int 链 1.5+2+10", [],
       '<bitdecimal>"1.5" + <int>2 + <int>10',
       N("+", N("+", ("bd", "1.5"), 2), 10))
    ec("bitdecimal 吸收 double（二进制精确 15 位）1.2+0.05", [],
       '<bitdecimal>"1.2" + <double>0.05',
       N("+", ("bd", "1.2"), ("dbl", 0.05)))
    ec("bitdecimal 吸收 decimal 1.2+0.005", [],
       '<bitdecimal>"1.2" + <decimal>"0.005"',
       N("+", ("bd", "1.2"), ("dec", "0.005")))
    ec("bigint 吸收 bitdecimal 截断 99999999999999999999+1.2", [],
       '<bigint>"99999999999999999999" + <bitdecimal>"1.2"',
       N("+", ("bi", 99999999999999999999), ("bd", "1.2")))
    ec("bitdecimal 链式 (1.2-0.3)*0.5+0.05", [],
       '(<bitdecimal>"1.2" - <bitdecimal>"0.3") * <bitdecimal>"0.5" + <bitdecimal>"0.05"',
       N("+", N("*", N("-", ("bd", "1.2"), ("bd", "0.3")), ("bd", "0.5")), ("bd", "0.05")))
    ec("bitdecimal 负数 -1.2-0.8", [],
       '<bitdecimal>"-1.2" - <bitdecimal>"0.8"',
       N("-", ("bd", "-1.2"), ("bd", "0.8")))
    ec("bitdecimal 高精度大数 123456789.123456789*2", [],
       '<bitdecimal>"123456789.123456789" * <bitdecimal>"2"',
       N("*", ("bd", "123456789.123456789"), ("bd", "2")))
    ec("string 与 bitdecimal 拼接 v=1.2", [],
       '"v=" + <bitdecimal>"1.2"', N("+", "v=", ("bd", "1.2")))
    ec("bitdecimal 小数前导零 0.0000001*10", [],
       '<bitdecimal>"0.0000001" * <bitdecimal>"10"',
       N("*", ("bd", "0.0000001"), ("bd", "10")))
    ec("bitdecimal 除法舍入进位 2/3", [],
       '<bitdecimal>"2" / <bitdecimal>"3"',
       N("/", ("bd", "2"), ("bd", "3")))

    # 把手工叶子规约成 oracle 操作数
    def edge_operands(decls, tree):
        vals = {}
        for i, (t, raw) in enumerate(decls):
            cat = category(t)
            if cat == CAT_DOUBLE:
                v = float(raw)
            elif t == "bool":
                v = 1 if raw == "true" else 0
            else:
                v = int(raw)
            vals[i] = (cat, t, raw, v)

        def conv(leaf):
            if isinstance(leaf, tuple):
                tag = leaf[0]
                x = leaf[1]
                if tag == "dec":
                    u, p = dec_parse(x)
                    return (CAT_DECIMAL, "decimal", x, (u, p))
                if tag == "bi":
                    return (CAT_BIGINT, "bigint", str(x), x)
                if tag == "bd":
                    return (CAT_BITDECIMAL, "bitdecimal", '"' + x + '"',
                            (Fraction(x), len(x.split(".")[1]) if "." in x else 0))
                if tag == "dbl":
                    return (CAT_DOUBLE, "double", repr(x), float(x))
                if tag == "bool":
                    return (CAT_INT, "bool", "true" if x else "false", x)
                if tag == "char":
                    return (CAT_INT, "char", chr(x), x)
                if tag == "intraw":
                    return (CAT_INT, "byte", str(x), x)
            if isinstance(leaf, bool):
                return (CAT_INT, "bool", "true" if leaf else "false", int(leaf))
            if isinstance(leaf, int):
                return (CAT_INT, "int", str(leaf), leaf)
            if isinstance(leaf, float):
                return (CAT_DOUBLE, "double", repr(leaf), leaf)
            if isinstance(leaf, str):
                return (CAT_STRING, "string", '"' + leaf + '"', leaf)
            raise ValueError(leaf)

        # 收集叶子（去重保序按 id）
        leaves = {}
        def walk(n):
            if isinstance(n, int):
                if n in vals:
                    leaves[n] = vals[n]
                else:
                    leaves[("L", n)] = conv(n)
                return
            _, l, r = n
            walk(l); walk(r)
        walk(tree)
        return leaves

    # ---------- 发射 .lm ----------
    lines = []
    expected = []
    lines.append("/* 多操作数链式混合运算测试：由 gen_chain_test.py 生成")
    lines.append(" * 操作数池来自 random_type_test.lm 的 1500 个随机类型案例")
    lines.append(" * 覆盖：3~5 操作数左结合链、优先级树、括号、四类算术与字符串运算")
    lines.append(" * 每行输出 C<id>:<结果类型>=<结果值>，与独立 oracle 期望差分比对 */")
    lines.append("")

    def emit_case(cid, decls, expr_src):
        for i, (tname, raw) in enumerate(decls):
            lines.append("v%d_%d = <%s>%s;" % (cid, i, tname, raw))
        lines.append("r%d = %s;" % (cid, expr_src))
        lines.append('print("C%d:" + type(r%d) + "=" + r%d);' % (cid, cid, cid))
        lines.append("")

    for cid, tree, arity, ops, cat, v in cases:
        decls = [(t, raw) for (_c, t, raw, _v) in ops]
        names = ["v%d_%d" % (cid, i) for i in range(arity)]
        expr = emit_node(tree, names)
        emit_case(cid, decls, expr)
        # long double 类型名：任一操作数是 long double 时，type() 返回 "long double"
        type_name = cat
        if cat == CAT_DOUBLE and any(o[1] == "long double" for o in ops):
            type_name = "long double"
        expected.append("C%d:%s=%s" % (cid, type_name, result_str(cat, v)))

    # 手工用例
    base = len(cases)
    for j, (desc, decls, expr_src, tree) in enumerate(edge_cases):
        cid = base + j + 1
        decls_out = list(decls)

        # tuple 叶子直接用其 raw 字面量
        def src_walk2(n):
            if isinstance(n, tuple):
                tag = n[0]; x = n[1]
                if tag == "dec":
                    return '<decimal>"%s"' % x
                if tag == "bi":
                    return '<bigint>"%d"' % x
                if tag == "bd":
                    return '<bitdecimal>"%s"' % x
                if tag == "dbl":
                    return "<double>" + repr(float(x))
                if tag == "bool":
                    return "true" if x else "false"
                if tag == "char":
                    return "<char>'" + chr(x) + "'"
                if tag == "intraw":
                    return "<byte>%d" % x
            if isinstance(n, bool):
                return "true" if n else "false"
            if isinstance(n, int):
                if 0 <= n < len(decls):
                    return "v%d_%d" % (cid, n)
                return str(n)
            if isinstance(n, float):
                return repr(n)
            if isinstance(n, str):
                return '"%s"' % n
            op, l, r = n
            return "(" + src_walk2(l) + " " + op + " " + src_walk2(r) + ")"

        lines.append("/* E%d: %s */" % (j + 1, desc))
        emit_case(cid, decls_out, src_walk2(tree))

        # oracle 期望
        operands_eval = {}
        for i, (t, raw) in enumerate(decls):
            cat2 = category(t)
            if cat2 == CAT_DOUBLE:
                vv = float(raw)
            elif t == "bool":
                vv = 1 if raw == "true" else 0
            else:
                vv = int(raw)
            operands_eval[i] = (cat2, t, raw, vv)

        def eval_with_const(n):
            if isinstance(n, tuple):
                tag, x = n[0], n[1]
                if tag == "dec":
                    u, p = dec_parse(x)
                    return (CAT_DECIMAL, (u, p))
                if tag == "bi":
                    return (CAT_BIGINT, x)
                if tag == "bd":
                    return (CAT_BITDECIMAL, (Fraction(x),
                            len(x.split(".")[1]) if "." in x else 0))
                if tag == "dbl":
                    return (CAT_DOUBLE, float(x))
                if tag == "bool":
                    return (CAT_INT, x)
                if tag == "char":
                    return (CAT_INT, x)
                if tag == "intraw":
                    return (CAT_INT, x)
            if isinstance(n, bool):
                return (CAT_INT, int(n))
            if isinstance(n, int):
                if n in operands_eval:
                    o = operands_eval[n]
                    return o[0], to_int64(o[3]) if o[0] == CAT_INT else o[3]
                return (CAT_INT, to_int64(n))
            if isinstance(n, float):
                return (CAT_DOUBLE, n)
            if isinstance(n, str):
                return (CAT_STRING, n)
            op, l, r = n
            cl, vl = eval_with_const(l)
            cr, vr = eval_with_const(r)
            res = promote(cl, cr)
            if res == CAT_STRING:
                return CAT_STRING, ptr_op(to_string(cl, vl), to_string(cr, vr), op)
            if res == CAT_BIGINT:
                bl = vl if cl == CAT_BIGINT else bigint_from_other(cl, vl)
                br = vr if cr == CAT_BIGINT else bigint_from_other(cr, vr)
                return CAT_BIGINT, {"+": bl + br, "-": bl - br, "*": bl * br,
                                    "/": trunc_div(bl, br)}[op]
            if res == CAT_BITDECIMAL:
                bl = vl if cl == CAT_BITDECIMAL else bd_from_value(cl, vl)
                br = vr if cr == CAT_BITDECIMAL else bd_from_value(cr, vr)
                return CAT_BITDECIMAL, bd_op(bl, br, op)
            if res == CAT_DECIMAL:
                dl = (vl[0], vl[1]) if cl == CAT_DECIMAL else dec_from_value(cl, vl)
                dr = (vr[0], vr[1]) if cr == CAT_DECIMAL else dec_from_value(cr, vr)
                return CAT_DECIMAL, dec_op(dl, dr, op)
            if res == CAT_DOUBLE:
                dl = float(to_int64(vl)) if cl == CAT_INT else vl
                dr = float(to_int64(vr)) if cr == CAT_INT else vr
                if op == "/" and dr == 0.0:
                    return CAT_DOUBLE, 0.0   # VM double 除零返回 0
                return CAT_DOUBLE, {"+": dl + dr, "-": dl - dr, "*": dl * dr, "/": dl / dr}[op]
            return CAT_INT, to_int64({"+": vl + vr, "-": vl - vr, "*": vl * vr,
                             "/": (0 if vr == 0 else trunc_div(vl, vr))}[op])

        cat3, v3 = eval_with_const(tree)
        expected.append("C%d:%s=%s" % (cid, cat3, result_str(cat3, v3)))

    with open(OUT_LM, "w", encoding="utf-8") as f:
        f.write("\n".join(lines))
    with open(OUT_EXPECT, "w", encoding="utf-8") as f:
        f.write("\n".join(expected) + "\n")

    # 统计
    from collections import Counter
    cnt = Counter(e.split(":")[1].split("=")[0] for e in expected)
    print("生成案例数:", len(expected))
    print("结果类型分布:", dict(cnt))
    print("源码:", OUT_LM)
    print("期望:", OUT_EXPECT)

if __name__ == "__main__":
    main()
