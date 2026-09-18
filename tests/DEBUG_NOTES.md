# lumin-lang 混合类型运算测试 Bug 排查笔记

## 问题描述

**现象**：
- 前 75 个混合类型运算测试只输出 64 行（R12-R75），前 11 个结果丢失
- 单独运行前 36 个、前 50 个都正常输出
- 完整 100 个测试从 R37 开始输出

**测试文件**：
- `tests/random_type_test.lm`：9001 行，1500 个随机类型混合运算测试
- `tests/test_mixed_75.lm`：75 个混合类型运算测试（当前调试目标）

## 已确认的事实

### 1. 指令序列分析
前 16 条指令（pc=0 到 pc=15）：
```
pc=0: PUSH_INT64_CONST   (压入 10)
pc=1: STORE_INT64_VAR    (存储到 a1)
pc=2: PUSH_INT64_CONST   (压入 7)
pc=3: PUSH_CONST_IDX     (从常量池加载!)
pc=4: BIGINT_FROM_STRING  (bigint 转换!) ← 这里不对!
pc=5: STORE_PTR_VAR      (存储到 PTR 栈!) ← 这里也不对!
pc=6: LOAD_INT64_VAR     (加载 a1)
pc=7: INT64_TO_STRING    (int 转字符串)
pc=8: BIGINT_FROM_STRING  (bigint 转换!)
pc=9: LOAD_PTR_VAR       (加载 PTR 变量)
pc=10: BIGINT_ADD        (bigint 相加!)
pc=11: STORE_PTR_VAR     (存储到 PTR 栈!)
pc=12: PUSH_CONST_IDX    (压入 "R12:")
pc=13: PUSH_CONST_IDX    (压入 "bigint")
pc=14: PTR_ADD           (字符串拼接)
pc=15: PRINT_PTR         (打印)
```

### 2. 关键发现
- `b1 = <double>7` 被错误地编译成了 **bigint**，而不是 double
- 应该生成 `OPC_INT64_TO_DOUBLE` + `OPC_STORE_DOUBLE_VAR`
- 实际生成了 `OPC_PUSH_CONST_IDX` + `OPC_BIGINT_FROM_STRING` + `OPC_STORE_PTR_VAR`

### 3. CastKind 枚举实际值（已确认）
```c
typedef enum {
    CAST_NONE,       // 0
    CAST_INT,        // 1
    CAST_DOUBLE,     // 2
    CAST_STRING,     // 3
    CAST_BOOL,       // 4
    CAST_ASCII,      // 5
    CAST_CHAR,       // 6
    CAST_BYTE,       // 7
    CAST_FUNC,       // 8
    CAST_ARRAY,      // 9
    CAST_MAP,        // 10
    CAST_ERROR,      // 11
    CAST_GENERATOR,  // 12
    CAST_STRUCT_PTR, // 13
    CAST_CLASS_PTR,  // 14
    CAST_TYPED_ARRAY,// 15
    CAST_INT8,       // 16
    CAST_INT16,      // 17
    CAST_INT32,      // 18
    CAST_INT64,      // 19
    CAST_UINT8,      // 20
    CAST_UINT16,     // 21
    CAST_UINT32,     // 22
    CAST_UINT,       // 23
    CAST_UINT64,     // 24
    CAST_LONG,       // 25
    CAST_LONGLONG,   // 26
    CAST_FLOAT,      // 27
    CAST_ULONG,      // 28
    CAST_UCHAR,      // 29
    CAST_SHORT,      // 30
    CAST_USHORT,     // 31
    CAST_SIZE_T,     // 32
    CAST_SSIZE_T,    // 33
    CAST_VOID,       // 34
    CAST_LONG_DOUBLE,// 35
    CAST_PTR,        // 36
    CAST_CALLBACK,   // 37
    CAST_BIGINT,     // 38
    CAST_DECIMAL,    // 39
} CastKind;
```

### 4. 调试输出验证
类型标注调试输出（前 6 个）：
```
DEBUG: TYPE_ANNOTATION: ct=3 (CAST_STRING), child_type=1
DEBUG: CAST_DOUBLE branch, child_type=1
DEBUG: emitting INT64_TO_DOUBLE
DEBUG: TYPE_ANNOTATION: ct=2 (CAST_DOUBLE), child_type=1
...
```

**注意**：调试输出中的类型标注顺序和测试文件中的顺序不一致！

## 排查方向

### 方向 1：词法分析器类型标注解析
- 文件：`src/parse/yacc.y` 第 1493 行 `TOK_TYPE_ANNOT unary_expr`
- 问题：`<double>` 是否被正确识别为 `TOK_TYPE_ANNOT`，值是否为 `CAST_DOUBLE`？
- 检查：词法分析器中 `<type>` 的解析逻辑，是否把 `double` 错误地映射成了 `CAST_BIGINT`？

### 方向 2：AST_TYPE_ANNOTATION 编译逻辑
- 文件：`src/ir/ir_compile.c` 第 173 行
- 问题：`ct == CAST_DOUBLE` 的判断是否正确？
- 已确认：CAST_DOUBLE 分支工作正常，会 emit `OPC_INT64_TO_DOUBLE`

### 方向 3：变量类型标记存储
- 文件：`src/ir/ir_compile.c` 第 665 行 `AST_ASSIGN` 分支
- 问题：赋值时是否正确存储了变量的类型标记？
- 检查：`get_var_cast_type()` 是否正确返回了变量的类型

### 方向 4：类型推断错误传播
- 文件：`src/ir/ir_arith.c`
- 问题：为什么 `a1 + b1` 的结果被推断成了 `bigint`？
- 检查：`arith_get_expr_type()` 的逻辑

## 关键文件

| 文件 | 作用 |
|------|------|
| `src/ir/bytecode_type.h` | OpCode 枚举定义 |
| `src/ir/ir_compile.c` | IR 编译器（类型标注处理在第 173 行） |
| `src/ir/ir_arith.c` | 算术运算类型推断 |
| `src/ir/vm_exec_arith.c` | VM 算术运算实现 |
| `src/ir/stack_manager.c` | 4 核心栈管理 |
| `src/parse/yacc.y` | 语法分析（类型标注在第 1493 行） |
| `src/ast/lumyr_types.h` | AST 节点类型定义 |

## 运行方式

```bash
# 编译
./concat_manifest.sh >/dev/null 2>&1 && make all

# 运行测试
./bin/lumyr tests/test_mixed_75.lm 2>/dev/null

# 带调试输出
./bin/lumyr tests/test_mixed_75.lm 2>&1 | grep "DEBUG:" | head -50
```

## 预期结果

前 75 个测试应该输出 75 行，每行格式：
```
R1:double
R2:bigint
R3:decimal
...
R75:bigint
```

## 当前结果

只输出 64 行，从 R12 开始：
```
R12:bigint
R13:decimal
...
R75:bigint
```

前 11 个结果（R1-R11）丢失。
