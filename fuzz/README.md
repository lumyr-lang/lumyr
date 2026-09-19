# Lumyr 模糊测试工具

## 目录结构

```
fuzz/
├── fuzz_runner.py        # 模糊测试主脚本（随机程序，VM/编译双模式差分）
├── gen_chain_test.py     # 链式混合运算生成器 + 独立 oracle（见下文）
├── cases/                # 临时测试用例目录
├── crashes/              # 崩溃/差异用例保存目录
└── README.md             # 本文档
```

## 链式混合运算差分测试（gen_chain_test.py）

从 `tests/random_type_test.lm` 的 1500 个二元运算案例提取操作数池，生成 318 个
3~5 操作数链式/混合运算案例（左结合链、`a+b*c`、`(a+b)*(c-d)` 等优先级形状，
以及除零、负数、字符串链、bigint 大整数、decimal 精度、bitdecimal 精度等边界
用例），并用独立 Python oracle 按 VM 语义计算每行期望的「类型+值」。固定随机
种子，结果可复现。bitdecimal 语义：512 位 GMP mpf_t，加减取 max 精度、乘法
精度相加、除法多保留 10 位，按 precision 固定小数位半偶舍入输出。

```bash
python3 fuzz/gen_chain_test.py
T="${TMPDIR:-/tmp}"
./bin/lumyr tests/chain_mixed_test.lm 2>/dev/null | grep -E '^C[0-9]+:' > "$T/chain_actual.txt"
diff "$T/chain_expected.txt" "$T/chain_actual.txt"   # 0 行即全部通过
```

## 使用方法

### 运行 100 个随机测试用例（VM + 编译模式）

```bash
python fuzz/fuzz_runner.py --count 100
```

### 指定随机种子（可复现）

```bash
python fuzz/fuzz_runner.py --count 50 --seed 42
```

### 只测试 VM 模式

```bash
python fuzz/fuzz_runner.py --vm-only --count 100
```

### 只测试编译模式

```bash
python fuzz/fuzz_runner.py --compile-only --count 100
```

## 测试策略

1. **随机程序生成**：自动生成包含变量、表达式、控制流（if/for/while）、函数调用的 Lumyr 程序
2. **双模式对比**：同时用 VM 模式和编译模式运行，比较输出是否一致（差分测试）
3. **崩溃检测**：检测非正常退出（返回码 < 0 或非语法错误的非零退出）
4. **超时检测**：每个测试用例超时时间为 10 秒（VM）/ 30 秒（编译）

## 崩溃用例分析

当检测到崩溃或输出不一致时，用例会保存到 `fuzz/crashes/` 目录：
- `<name>.lm`：触发问题的 Lumyr 源代码
- `<name>.txt`：详细的运行信息（返回码、stdout、stderr）

### 复现崩溃

```bash
# VM 模式复现
./bin/lumyr fuzz/crashes/crash_0001.lm

# 编译模式复现
./bin/lumyr -c fuzz/crashes/crash_0001.lm -o _crash
./_crash.exe
```

## 扩展模糊测试

可以通过修改 `LumyrGenerator` 类来扩展测试覆盖：
- 添加更多语言特性（函数定义、类、异常处理等）
- 调整表达式深度和语句数量
- 添加特定领域的测试模式（如 GC 压力、并发测试）

## 注意事项

- Windows 杀毒软件可能会删除新编译的 .exe 文件，如遇问题请添加排除项
- 编译模式包含 gcc 编译时间，单测耗时较长
- 建议在空闲系统上运行，避免其他进程影响结果
- 模糊测试发现的问题应最小化用例后提交到 issue 跟踪系统
