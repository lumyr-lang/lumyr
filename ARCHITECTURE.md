# Lumyr 语言架构设计文档

> 版本：v1.0  
> 定位：性能对标 C 语言的高级系统编程语言，支持解释执行 + 编译执行双通道  
> 设计目标：高并发、多线程多进程、无硬上限、零类型转换开销

---

## 1. 设计理念

### 1.1 核心目标

| 目标 | 实现路径 |
|------|----------|
| **性能对标 C** | 专用类型栈 + 逃逸分析 + 栈分配 + 标量替换 |
| **高并发** | 线程本地栈帧 + 无锁分配 + 真 STW GC + TLA |
| **多线程多进程** | pthread 封装 + 全套锁机制 + 条件变量 + 线程局部存储 |
| **无硬上限** | 所有动态结构按需扩容，无固定大小限制 |
| **零类型转换** | 一对一类型对齐 + 专用指令 + 宽槽栈帧设计 |

### 1.2 双通道架构

```
┌─────────────────────────────────────────────────────────┐
│                     源码 (.lm)                          │
└────────────────────┬────────────────────────────────────┘
                      │
         ┌────────────▼────────────┐
         │    Lexer + Parser        │
         │    (flex + bison)        │
         └────────────┬────────────┘
                      │
         ┌────────────▼────────────┐
         │    AST 构建              │
         │    (ast_node.c)          │
         └────────────┬────────────┘
                      │
         ┌────────────▼────────────┐
         │    语义检查 / 类型推导    │
         │    (ast_typecheck.c)     │
         └────────────┬────────────┘
                      │
         ┌────────────▼────────────┐
         │    IR 编译               │
         │    (ir_compile.c)        │
         │    生成字节码 + 栈深分析  │
         └────────────┬────────────┘
                      │
          ┌───────────┴───────────┐
          ▼                       ▼
┌─────────────────┐     ┌─────────────────┐
│   VM 解释器     │     │   C 编译器      │
│   (vm_exec.c)   │     │   (ir_cgen.c)   │
│   字节码执行     │     │   生成 C 代码    │
│   4 核心栈       │     │   gcc 编译       │
└─────────────────┘     └─────────────────┘
```

---

## 2. 类型系统

### 2.1 类型分类

```
基础类型
├── 整数类型
│   ├── 有符号: int8, int16, int32, int64, short, int, long, long long
│   └── 无符号: uint8, uint16, uint32, uint64, uchar, ushort, uint, ulong
├── 浮点类型
│   ├── float (32位单精度)
│   ├── double (64位双精度)
│   └── long double (扩展精度)
├── 字符类型
│   ├── char
│   └── byte (uint8)
├── 布尔类型: bool
├── 字符串类型: string (引用语义)
└── 指针类型: void*

复合类型
├── 数组: 支持泛型 <T>[]、多维数组、栈分配/堆分配
├── 字典: map<K,V>，哈希表 + 红黑树自动升级
├── 结构体: type 声明，支持 __classname__ 只读属性
├── 类: class 声明，vtable 虚函数表
├── 函数: 支持闭包、匿名函数、高阶函数
└── 错误对象: try/catch/finally 异常机制
```

### 2.2 一对一类型对齐原则

> **设计铁律**：每个 C 类型对应唯一的 Value 联合体字段，禁止混用

```
Value 联合体字段映射:
┌─────────────┬─────────────┬──────────────────┐
│ CastKind    │ Value.type  │ 联合体字段        │
├─────────────┼─────────────┼──────────────────┤
│ CAST_INT    │ VAL_INT     │ v.i (int, 32位)  │
│ CAST_INT8   │ VAL_INT8    │ v.i8 (int8_t)    │
│ CAST_INT16  │ VAL_INT16   │ v.i16 (int16_t)  │
│ CAST_INT32  │ VAL_INT32   │ v.i32 (int32_t)  │
│ CAST_INT64  │ VAL_INT64   │ v.i64 (int64_t)  │
│ CAST_SHORT  │ VAL_SHORT   │ v.sh (short)     │
│ CAST_LONG   │ VAL_LONG    │ v.l (long)       │
│ CAST_LONGLONG│VAL_LONG_LONG│v.ll (long long)  │
│ CAST_UINT8  │ VAL_UINT8   │ v.u8 (uint8_t)   │
│ CAST_UINT16 │ VAL_UINT16  │ v.u16 (uint16_t) │
│ CAST_UINT32 │ VAL_UINT32  │ v.u32 (uint32_t) │
│ CAST_UINT64 │ VAL_UINT64  │ v.u64 (uint64_t) │
│ CAST_FLOAT  │ VAL_FLOAT   │ v.f (float)      │
│ CAST_DOUBLE │ VAL_DOUBLE  │ v.d (double)     │
│ CAST_BOOL   │ VAL_BOOL    │ v.b (_Bool)      │
│ CAST_CHAR   │ VAL_CHAR    │ v.c (char)       │
│ CAST_BYTE   │ VAL_BYTE    │ v.by (byte)      │
└─────────────┴─────────────┴──────────────────┘
```

---

## 3. 栈架构（核心性能设计）

### 3.1 四核心栈设计

> 从 24 个独立专用栈合并为 4 个宽槽栈，与 StackFrame 宽槽设计对齐

```
┌─────────────────────────────────────────────────────┐
│                    执行栈系统                        │
├──────────────┬──────────────┬───────────────────────┤
│  栈名        │  元素类型    │  用途                │
├──────────────┼──────────────┼───────────────────────┤
│  __stk       │  Value       │  通用动态值栈         │
│  __int_stack │  int64_t     │  所有整数统一栈       │
│  __double_stack│ double     │  所有浮点统一栈       │
│  __ptr_stack │  void*       │  字符串/指针栈        │
└──────────────┴──────────────┴───────────────────────┘
```

**设计优势：**
- 内存占用降至原来的 1/5
- malloc 次数从 20+ 次降至 4 次
- CPU 缓存命中率大幅提升
- 类型截断/扩展由 C 编译器自动处理，零额外开销

### 3.2 栈帧（StackFrame）宽槽设计

> 从 20+ 个独立数组合并为 3 个统一槽，消除冗余开销

```
StackFrame 结构:
┌─────────────────────────────────────────┐
│  names[]     // 变量名数组               │
│  vals[]      // Value 通用值数组         │
│  int_slots[] // int64_t 统一整数槽       │
│  flt_slots[] // double 统一浮点槽        │
│  type_tags[] // CastKind 类型标记数组    │
│  parent      // 父栈帧指针               │
│  shared      // 是否全局共享帧           │
│  rw          // 读写锁（共享帧用）        │
└─────────────────────────────────────────┘
```

**关键优化：**
- 私有帧：无锁，线程本地
- 共享帧：读写锁 + 句柄间接层（扩容时原子替换，避免 UAF）
- type_tags 用 uint8_t 存储（枚举值 < 256）

---

## 4. 内存管理

### 4.1 GC 架构

```
┌─────────────────────────────────────────────────────┐
│                    GC 系统                           │
├─────────────────────────────────────────────────────┤
│  算法: 标记-清除 (Mark-Sweep)                       │
│  阈值: 初始 64MB，GC 后 = max(当前用量×2, 1MB)      │
│  根扫描: VM 栈 + 帧链局部变量 (TLS)                 │
│  线程安全: 全局互斥锁 + 真 STW 确认机制             │
└─────────────────────────────────────────────────────┘
```

### 4.2 关键 GC 优化

| 优化 | 说明 | 收益 |
|------|------|------|
| **TLA** | 小对象 (<=256B) 走线程本地空闲链表 | 无锁分配，多线程快 22% |
| **新对象预标记** | 新分配对象 marked=3，STW 时不被误扫 | 消除 torn Value 竞态 |
| **字符串 SSO** | <=22 字节字符串内联在 Value 结构 | 零堆分配，短字符串 GC 降 100% |
| **栈分配** | 逃逸分析判定不逃逸的对象栈上分配 | 零 GC 压力 |
| **标量替换** | 固定下标数组/固定键 map 拆解为标量变量 | 连结构体都不需要 |
| **真 STW** | at_safepoint 标志 + sched_yield 轮询确认 | 彻底消除多线程 UAF |

### 4.3 栈分配链（逃逸分析）

```
变量声明
    │
    ▼
逃逸分析（位掩码流敏感）
    │
    ├── 不逃逸 ──→ ValueArray 结构体栈分配
    │                  │
    │                  ├── 固定大小 items ──→ items 缓冲区栈分配
    │                  │
    │                  └── 固定下标访问 ──→ 标量替换（拆解为独立变量）
    │
    └── 逃逸 ──→ GC 堆分配
```

---

## 5. 并发系统

### 5.1 线程模型

```
┌─────────────────────────────────────────────────────┐
│                   并发原语                           │
├──────────────┬──────────────────────────────────────┤
│  互斥锁      │ mutex() / lock() / unlock()          │
│  递归锁      │ rmutex() / rlock() / runlock()       │
│  读写锁      │ rwlock() / rlock() / wlock()         │
│  自旋锁      │ spinlock() / spinlock_lock()         │
│  条件变量    │ cond_wait() / cond_signal()          │
│  非阻塞尝试  │ tryrlock() / trywlock() / trylock()  │
│  线程局部    │ threadlocal() 变量                   │
│  线程创建    │ thread(func, args)                   │
│  线程等待    │ thread_join(t)                       │
└──────────────┴──────────────────────────────────────┘
```

### 5.2 多线程 GC 安全

**6 层纵深防御：**

1. **全局线程栈注册表** — 所有线程栈注册到全局表
2. **新对象预标记** — 新分配对象 marked=3，STW 时不被误扫
3. **realloc → malloc+memcpy** — 避免 realloc 移动内存导致指针失效
4. **VM 栈 pop-push 写入** — 栈操作原子化，避免 torn read
5. **协作式 STW** — g_gc_stw 标志 + at_safepoint 确认
6. **指针合法性校验** — GC 标记时验证指针是否在 GC 链表中

---

## 6. 内置函数与标准库

### 6.1 类型系统内置

```
type(x)        // 返回类型名字符串
len(x)         // 数组/字符串/字典长度
str(x)         // 转字符串
int(x)         // 转整数
float(x)       // 转浮点数
bool(x)        // 转布尔
```

### 6.2 字符串操作

```
s[i]           // 按下标读字符
len(s)         // 字符串长度
substr(s, i, n) // 字符串切片
s.encode(encoding)   // 字符编码转换
s.decode(encoding)
s.toUpperCase() / s.toLowerCase()
s.split(sep)
s.replace(old, new)
s.repeat(n)
s.strip() / s.startsWith(prefix)
s.encodeURL() / s.decodeURL()
s.md5() / s.encodeBase64() / s.decodeBase64()
```

### 6.3 数组方法链

```
arr.add(x)           // 末尾添加
arr.insert(i, x)     // 指定位置插入
arr.remove(i)        // 删除元素
arr.get(i)          // 读取元素
arr.indexOf(x)       // 查找元素下标
arr.contains(x)     // 是否包含
arr.clear()         // 清空
arr.addAll(other)   // 批量添加
arr.flat()          // 扁平化（8维测试通过）
arr.map(fn)         // 映射
arr.filter(fn)      // 过滤
arr.reduce(fn, init) // 聚合
arr.sort()          // 排序
arr.reverse()       // 反转
arr.sum() / arr.avg() // 聚合统计
```

### 6.4 字典方法链

```
map.get(key)        // 读取
map.set(key, val)   // 写入
map.has(key)        // 是否存在
map.remove(key)     // 删除
map.clear()         // 清空
map.keys() / map.values()
map.addAll(other)
map.qs.parse(qs)   // 解析查询字符串
map.qs.stringify()  // 序列化为查询字符串
map.json.parse(s)   // JSON 反序列化
map.json.stringify() // JSON 序列化
```

### 6.5 系统功能

```
requests.get(url, params, headers)   // HTTP GET
requests.post(url, body, headers)   // HTTP POST
read(path) / write(path, content)   // 文件 IO
regex.match(pattern, s)             // 正则匹配
regex.replace(pattern, s, repl)     // 正则替换
date.now() / date.format(ts, fmt)   // 日期时间
log.debug/info/warn/error(msg)      // 日志系统
```

---

## 7. 错误处理

### 7.1 异常机制

```
try {
    // 可能出错的代码
    throw Error("自定义错误");
} catch (e) {
    // e.type: 错误类型
    // e.message: 错误消息
    // e.stack: 栈回溯
} finally {
    // 无论是否出错都执行
}
```

### 7.2 错误对象结构

```
Error {
    type: string      // 错误类型
    message: string   // 错误消息
    stack: string     // 栈回溯（自动生成）
}
```

---

## 8. 性能基准

> 基于真实 benchmark 测试

| 场景 | 优化前 | 优化后 | 加速比 |
|------|--------|--------|--------|
| 混合运算 | 1.0x | 4.0x | **4.0x** |
| Map 操作 | 1.0x | 1.47x | 1.47x |
| 数组操作 | 1.0x | 0.98x | ~1.0x |
| 字符串操作 | 1.0x | 0.85x | 0.85x (SSO 分支开销) |
| GC 压力 | 1.0x | 0.85x | 0.85x |
| 多线程分配 | 1.0x | 0.78x | **1.28x** |

---

## 9. 已知技术债与后续方向

### 9.1 待优化

| 项目 | 优先级 | 说明 |
|------|--------|------|
| 字符串 SSO 分支开销 | P1 | SSO 访问路径分支判断成为净开销 |
| 增量标记 | P2 | 需要写屏障维护三色不变式，风险高 |
| 分代 GC | P2 | 新生代小对象频繁回收，老年代少回收 |
| 编译通道重写 | P1 | 对齐新栈设计后重新生成 C 代码 |
| VM 执行器重写 | P0 | 对齐新栈设计后重新实现解释器 |

### 9.2 后续功能方向

- 模块系统（import/export）
- 包管理
- JIT 编译器
- 协程/异步 IO
- 标准库完善
- IDE 工具链

---

## 10. 文件结构

```
lumin-lang-compiler/
├── src/
│   ├── lex/           # 词法分析 (flex)
│   ├── parse/         # 语法分析 (bison)
│   ├── ast/           # AST 构建与语义检查
│   ├── ir/            # IR 中间表示
│   │   ├── bytecode.h     # 字节码定义
│   │   ├── stack_manager.h # 栈管理器
│   │   ├── ir_compile.c   # IR 编译器
│   │   ├── ir_arith.c     # 算术运算优化
│   │   ├── vm.c           # VM 入口（待重写）
│   │   └── ir_cgen.c      # C 代码生成（待重写）
│   └── runtime/       # 运行时
├── kit/runtime/
│   ├── include/       # 运行时头文件
│   └── src/           # 运行时实现
├── tests/             # 测试用例
├── benchmarks/        # 性能基准
└── Makefile
```

---

## 附录 A: 设计原则总结

1. **零转换开销**：类型一对一，专用指令，专用栈
2. **无硬上限**：所有动态结构按需扩容，对标 C
3. **高并发安全**：6 层纵深防御，真 STW，TLA 无锁分配
4. **双通道一致**：VM 和编译通道语义完全一致
5. **性能优先**：逃逸分析 + 栈分配 + 标量替换组合拳
6. **向后兼容**：保留所有头文件结构体和枚举定义
