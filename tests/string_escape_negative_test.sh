#!/bin/sh
# 字符串/字符转义负面用例回归：每个用例必须编译失败且输出中英双语诊断。
# 用法：sh tests/string_escape_negative_test.sh
# 从仓库根目录运行（与 ./bin/lumyr 同级）。
LUMYR=./bin/lumyr
TMP=$(mktemp -d /tmp/lm_esc_neg.XXXXXX)
trap 'rm -rf "$TMP"' EXIT
fail=0

# run_case <名称> <期望输出须包含的文本> <<'LM' ... LM
run_case() {
    name=$1; need=$2
    out=$($LUMYR "$TMP/case.lm" 2>&1)
    rc=$?
    if [ $rc -eq 0 ]; then
        echo "  FAIL $name: 预期编译失败但成功退出 / expected failure but got rc=0"
        fail=1
        return
    fi
    case "$out" in
        *"$need"*) ;;
        *)
            echo "  FAIL $name: 输出缺少 [$need]，实际：$out"
            fail=1
            return
            ;;
    esac
    echo "  ok   $name"
}

cat > "$TMP/case.lm" <<'EOF'
x = "\q";
EOF
run_case "未知转义 / unknown escape" "invalid string escape"

cat > "$TMP/case.lm" <<'EOF'
x = "\x";
EOF
run_case "\\x 截断 / truncated \\x" "requires at least 1 hexadecimal digit"

cat > "$TMP/case.lm" <<'EOF'
x = "\xG";
EOF
run_case "\\x 非十六进制 / non-hex \\x" "requires at least 1 hexadecimal digit"

cat > "$TMP/case.lm" <<'EOF'
x = "\u12";
EOF
run_case "\\u 位数不足 / short \\u" "requires exactly 4 hexadecimal digits"

cat > "$TMP/case.lm" <<'EOF'
x = "\uD800";
EOF
run_case "\\u 代理区 / surrogate" "invalid unicode code point"

cat > "$TMP/case.lm" <<'EOF'
x = "\U00110000";
EOF
run_case "\\U 超范围 / out of range" "invalid unicode code point"

cat > "$TMP/case.lm" <<'EOF'
x = "\400";
EOF
run_case "八进制 >255 / octal >255" "out of byte range"

cat > "$TMP/case.lm" <<'EOF'
x = 'ab';
EOF
run_case "char 两字符 / multi-char literal" "exactly one character"

cat > "$TMP/case.lm" <<'EOF'
x = '\u0041';
EOF
run_case "char 不支持 Unicode / unicode in char" "unicode escape not allowed in char literal"

if [ $fail -ne 0 ]; then
    echo "===== string escape negative FAILED / 负面用例存在失败 ====="
    exit 1
fi
echo "===== string escape negative ALL PASS (9/9) / 转义负面用例全部通过 ====="
