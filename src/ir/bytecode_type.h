// lumyr-lang 字节码类型定义
// 单独抽离：OpCode 枚举、Instruction 结构体、BuiltinId 枚举、BytecodeFunc 结构体
#ifndef LUMYR_IR_BYTECODE_TYPE_H
#define LUMYR_IR_BYTECODE_TYPE_H

#include "lumyr_value_type.h"
#include "lumyr_value.h"

/* ============================================================
 * OpCode 枚举：所有字节码指令
 * ============================================================ */
/* ============================================================
 * OpCode 枚举：所有字节码指令（4 核心栈设计）
 * 
 * 栈设计：
 *   STACK_VALUE  - 通用 Value 栈（动态类型、对象、字符串堆指针）
 *   STACK_INT64  - 统一整数栈（所有整数类型、bool、char 都存 int64_t）
 *   STACK_DOUBLE - 统一浮点栈（float、double、long double 都存 double）
 *   STACK_PTR    - 指针栈（字符串 SSO 内联、对象指针、FFI 指针）
 *
 * 设计原则：
 *   - 类型识别完整（CastKind 枚举保留）
 *   - 指令合并精简（4 核心栈对应 4 组指令）
 *   - 类型截断/扩展由 C 编译器自动处理
 * ============================================================ */
typedef enum {
    /* ===== 栈操作（通用） ===== */
    OPC_NOP,
    OPC_POP,           // 丢弃栈顶
    OPC_DUP,           // 复制栈顶
    OPC_TO_BOOL,       // 弹1压1 bool（Value 栈）

    /* ===== 常量加载 ===== */
    OPC_LOAD_CONST,    // a=常量池下标：从常量池加载 Value，压入 Value 栈
    OPC_PUSH_INT64_CONST,   // a=值（int32 范围小常量内嵌）：压入 int64 栈
    OPC_PUSH_CONST_IDX,     // a=常量池索引：从统一常量池加载大常量（int64/uint64/double/string）
    OPC_PUSH_DOUBLE_CONST,  // a=低32位, b=高32位：压入 double 栈
    OPC_PUSH_PTR_CONST,     // a=指针常量值：压入指针栈（字符串、对象指针）
    OPC_LOAD_STRING_CONST,  // a=字符串常量池索引：压字符串指针到 PTR 栈

    /* ===== 变量加载/存储（4 核心栈对应） ===== */
    OPC_LOAD_VAR,          // a=符号表下标：加载 Value 变量，压入 Value 栈
    OPC_STORE_VAR,         // a=符号表下标：从 Value 栈弹值，存储到变量
    OPC_LOAD_INT64_VAR,    // a=符号表下标：加载 int64 变量，压入 int64 栈
    OPC_STORE_INT64_VAR,   // a=符号表下标：从 int64 栈弹值，存储到变量
    OPC_LOAD_DOUBLE_VAR,   // a=符号表下标：加载 double 变量，压入 double 栈
    OPC_STORE_DOUBLE_VAR,  // a=符号表下标：从 double 栈弹值，存储到变量
    OPC_LOAD_PTR_VAR,      // a=符号表下标：加载指针变量，压入指针栈
    OPC_STORE_PTR_VAR,     // a=符号表下标：从指针栈弹值，存储到变量
    OPC_LOAD_VAR_REF,      // a=符号表下标：加载 ref 参数（struct 不转 Map，直接传递指针）

    /* ===== Value 栈算术运算（通用动态类型） ===== */
    OPC_ADD, OPC_SUB, OPC_MUL, OPC_DIV, OPC_MOD,
    OPC_GT, OPC_LT, OPC_GE, OPC_LE, OPC_EQ, OPC_NE, OPC_IMPLEMENTS,
    OPC_NEG, OPC_POS,
    OPC_LOGIC_NOT,       // 弹1压1 bool 取反
    OPC_PRE_INC, OPC_POST_INC, OPC_PRE_DEC, OPC_POST_DEC,  // a=符号表下标

    /* ===== int64 栈算术运算（统一整数栈，零检查零转换） ===== */
    OPC_INT64_ADD,       // 弹2个 int64，相加，结果压回 int64 栈
    OPC_INT64_SUB,      // 弹2个 int64，相减
    OPC_INT64_MUL,      // 弹2个 int64，相乘
    OPC_INT64_DIV,      // 弹2个 int64，相除（检查除零）
    OPC_INT64_MOD,      // 弹2个 int64，取模
    OPC_INT64_GT,       // 弹2个 int64，大于比较，结果 bool 压入 Value 栈
    OPC_INT64_LT,       // 小于
    OPC_INT64_GE,       // 大于等于
    OPC_INT64_LE,       // 小于等于
    OPC_INT64_EQ,       // 等于
    OPC_INT64_NE,       // 不等于
    OPC_INT64_TO_VALUE, // 从 int64 栈弹出，包装成 Value，压入 Value 栈（兼容赋值等通用逻辑）

    /* ===== double 栈算术运算（统一浮点栈，零检查零转换） ===== */
    OPC_DOUBLE_ADD,     // 弹2个 double，相加，结果压回 double 栈
    OPC_DOUBLE_SUB,     // 弹2个 double，相减
    OPC_DOUBLE_MUL,     // 弹2个 double，相乘
    OPC_DOUBLE_DIV,     // 弹2个 double，相除（检查除零）
    OPC_DOUBLE_GT,      // 弹2个 double，大于比较，结果 bool 压入 Value 栈
    OPC_DOUBLE_LT,      // 小于
    OPC_DOUBLE_GE,      // 大于等于
    OPC_DOUBLE_LE,      // 小于等于
    OPC_DOUBLE_EQ,      // 等于
    OPC_DOUBLE_NE,      // 不等于
    OPC_DOUBLE_TO_VALUE, // 从 double 栈弹出，包装成 Value，压入 Value 栈

    /* ===== ptr 栈算术运算（字符串拼接等） ===== */
    OPC_PTR_ADD,        // 弹2个指针（字符串），拼接，结果压回 ptr 栈
    OPC_PTR_MUL,        // 字符串乘法："abc" * 3 = "abcabcabc"
    OPC_PTR_DIV,        // 字符串除法："abcabcabc" / 3 = "abc"
    OPC_PTR_SUB,        // 字符串减法："abcabc" - 3 = "abc"（尾部截取），3 - "abcabc" = "abc"（首部截取）

    /* ===== 栈间转换（零包装零 Value 开销） ===== */
    OPC_INT64_TO_DOUBLE,    // int64 → double
    OPC_DOUBLE_TO_INT64,    // double → int64（截断）
    OPC_INT64_TO_PTR,       // int64 → ptr（指针运算）
    OPC_PTR_TO_INT64,       // ptr → int64（指针比较）
    OPC_INT64_TO_STRING,    // int64 → string（int64 栈弹出，转字符串，压入 PTR 栈）
    OPC_DOUBLE_TO_STRING,   // double → string（double 栈弹出，转字符串，压入 PTR 栈）

    /* ===== 类型转换（Value 栈内，通用） ===== */
    OPC_CAST_INT, OPC_CAST_DOUBLE, OPC_CAST_CHAR, OPC_CAST_BOOL, OPC_CAST_STRING, OPC_CAST_ASCII, OPC_CAST_BYTE,
    OPC_CAST_INT8, OPC_CAST_INT16, OPC_CAST_INT32, OPC_CAST_INT64,
    OPC_CAST_UINT8, OPC_CAST_UINT16, OPC_CAST_UINT32, OPC_CAST_UINT64,
    OPC_CAST_LONG, OPC_CAST_LONGLONG, OPC_CAST_FLOAT,

    /* ===== bigint 任意精度整数 ===== */
    OPC_BIGINT_FROM_STRING,  // 从 PTR 栈弹字符串指针，转 bigint 对象，压回 PTR 栈
    OPC_BIGINT_ADD,          // 从 PTR 栈弹 2 个 bigint 指针，相加，结果压回 PTR 栈
    OPC_BIGINT_SUB,          // 从 PTR 栈弹 2 个 bigint 指针，相减，结果压回 PTR 栈
    OPC_BIGINT_MUL,          // 从 PTR 栈弹 2 个 bigint 指针，相乘，结果压回 PTR 栈
    OPC_BIGINT_DIV,          // 从 PTR 栈弹 2 个 bigint 指针，相除，结果压回 PTR 栈
    OPC_BIGINT_TO_STRING,    // 从 PTR 栈弹 bigint 指针，转字符串，压回 PTR 栈

    /* ===== decimal 高精度十进制浮点 ===== */
    OPC_DECIMAL_FROM_STRING,  // 从 PTR 栈弹字符串指针，转 decimal 对象，压回 PTR 栈
    OPC_BITDECIMAL_FROM_STRING, // 从 PTR 栈弹字符串指针，转 bitdecimal 对象（基于 GMP mpf_t），压回 PTR 栈
    OPC_DECIMAL_ADD,           // 从 PTR 栈弹 2 个 decimal 指针，相加，结果压回 PTR 栈
    OPC_DECIMAL_SUB,          // 从 PTR 栈弹 2 个 decimal 指针，相减，结果压回 PTR 栈
    OPC_DECIMAL_MUL,           // 从 PTR 栈弹 2 个 decimal 指针，相乘，结果压回 PTR 栈
    OPC_DECIMAL_DIV,          // 从 PTR 栈弹 2 个 decimal 指针，相除，结果压回 PTR 栈
    OPC_DECIMAL_TO_STRING,    // 从 PTR 栈弹 decimal 指针，转字符串，压回 PTR 栈

    /* ===== bitdecimal 高精度十进制浮点（基于 GMP mpf_t） ===== */
    OPC_BITDECIMAL_TO_STRING,   // 从 PTR 栈弹 bitdecimal 指针，转字符串，压回 PTR 栈
    OPC_BITDECIMAL_FROM_INT64,  // 从 INT64 栈弹整数，转 bitdecimal 对象，压回 PTR 栈
    OPC_BITDECIMAL_FROM_DOUBLE, // 从 DOUBLE 栈弹浮点，转 bitdecimal 对象，压回 PTR 栈
    OPC_BITDECIMAL_ADD,         // 从 PTR 栈弹 2 个 bitdecimal 指针，相加，结果压回 PTR 栈
    OPC_BITDECIMAL_SUB,         // 从 PTR 栈弹 2 个 bitdecimal 指针，相减，结果压回 PTR 栈
    OPC_BITDECIMAL_MUL,         // 从 PTR 栈弹 2 个 bitdecimal 指针，相乘，结果压回 PTR 栈
    OPC_BITDECIMAL_DIV,         // 从 PTR 栈弹 2 个 bitdecimal 指针，相除，结果压回 PTR 栈

    /* ===== 数组/字典字面量 ===== */
    OPC_ARRAY_LIT,      // b=元素个数；弹 b 个 Value 元素，压数组
    OPC_INT64_ARRAY_LIT,   // b=元素个数；弹 b 个 int64 元素，压入 int64 类型化数组
    OPC_DOUBLE_ARRAY_LIT,  // b=元素个数；弹 b 个 double 元素，压入 double 类型化数组
    OPC_PTR_ARRAY_LIT,    // b=元素个数；弹 b 个指针元素，压入指针类型化数组
    OPC_MAP_LIT,        // b=键值对个数；弹 2b 个值（键、值交替）压字典

    /* ===== 下标访问/赋值 ===== */
    OPC_INDEX_GET,      // 弹 arr,idx 压元素（数组元素 / 字符串字符 / 字典键）
    OPC_INDEX_SET,      // 弹 arr,idx,val 写回；压回 val（表达式值）
    OPC_INT64_INDEX_SET,   // 从 Value 栈弹数组和索引，从 int64 栈弹值，写入 int64 类型化数组
    OPC_DOUBLE_INDEX_SET,  // 从 Value 栈弹数组和索引，从 double 栈弹值，写入 double 类型化数组

    /* ===== 结构体字段访问 ===== */
    OPC_LOAD_FIELD,     // a=变量符号下标，b=字段名常量下标；直接加载 struct 字段（零开销）
    OPC_STORE_FIELD,    // a=变量符号下标，b=字段名常量下标；弹值写入 struct 字段，压回值
    OPC_STORE_NESTED_FIELD, // a=变量符号下标，b=组合字段名常量下标；弹值写入嵌套 struct 字段
    OPC_LOAD_STRUCT_PTR,   // a=变量索引；加载 struct 变量的指针（用于方法 self 参数）

    /* ===== 内置函数调用 ===== */
    OPC_BUILTIN,        // a=内置函数 ID，b=实参个数（见 BuiltinId）

    /* ===== 打印（4 核心栈对应） ===== */
    OPC_PRINT,          // 打印 Value 栈顶，不弹出
    OPC_PRINT_INT64,    // 从 int64 栈弹出并打印（零开销）
    OPC_PRINT_DOUBLE,   // 从 double 栈弹出并打印（零开销）
    OPC_PRINT_PTR,      // 从指针栈弹出并打印（零开销）
    OPC_PRINT_BIGINT,   // 从指针栈弹出 bigint 对象并打印
    OPC_PRINT_DECIMAL,  // 从指针栈弹出 decimal 对象并打印

    /* ===== 错误处理（try/catch/finally） ===== */
    OPC_TRY,            // a=catch 起始pc(0=无catch)，b=finally 起始pc(0=无finally)；setjmp 注册错误处理器
    OPC_ENDTRY,         // a=跳转目标pc；正常路径恢复外层处理器
    OPC_GET_ERR,        // 压入最近捕获的错误对象（type/message/stack）
    OPC_THROW,          // 弹1；包装成错误对象并抛出（无处理器则打印退出）
    OPC_FIN_PUSH,       // a=完成动作(1=JMP 2=RETHROW 3=BREAK 4=CONT)，b=目标pc；压入 finally 完成动作
    OPC_FINISH,         // 弹 finally 完成动作并执行（JMP/RETHROW/RETURN 恢复）
    OPC_PEND_RETURN,    // 弹1（返回值）→ 挂起返回动作，跳 b（finally 起始；0=直接返回）

    /* ===== 控制流 ===== */
    OPC_JMP,            // a=目标pc
    OPC_JMP_IF_FALSE,   // a=目标pc；弹条件，假则跳
    OPC_JMP_IF_TRUE,    // a=目标pc；弹条件，真则跳
    OPC_JMP_IF_NULL,    // a=目标pc；弹值，为 VAL_NONE 则跳

    /* ===== 函数调用/返回 ===== */
    OPC_GETFUNC,        // a=函数名符号下标：压入函数值
    OPC_CALL,           // a=函数名符号下标，b=实参个数
    OPC_CALLV,          // 动态调用链：栈顶下一位=函数值（b=实参个数），栈顶 b 个为实参
    OPC_MKCLOSURE,      // a=lambda 符号下标：沿当前帧装箱捕获变量，压入新闭包函数值
    OPC_RETURN,         // 弹值返回（深拷贝）
    OPC_RETURN_NIL,     // 无返回值返回
    OPC_YIELD,          // 生成器 yield：弹值，保存执行状态，返回给调用者

    /* ===== 类 ===== */
    OPC_CLASS_NEW,      // a=常量池下标（RuntimeTypeInfo* 存为 CONST_UINT64）：创建实例压 PTR 栈
    OPC_CALL_METHOD,    // a=方法名常量下标，b=实参个数（不含 self）；多态分派

    OPC_HALT,

    /* ===== 通用 Value 运算（动态类型兜底：无类型标注参数/变量，运行时按 Value.type 分派） ===== */
    OPC_VADD,           // VALUE 栈弹2 → lumyr_add → 压结果
    OPC_VSUB, OPC_VMUL, OPC_VDIV, OPC_VMOD,
    OPC_VNEG,           // VALUE 栈弹1 → lumyr_unary_minus
    OPC_VGT, OPC_VLT, OPC_VGE, OPC_VLE, OPC_VEQ, OPC_VNE, // 弹2 → bool Value
    OPC_JMP_IF_TRUE_V,  // 条件在 VALUE 栈（truthy 判断）
    OPC_JMP_IF_FALSE_V,

    /* ===== typed 栈 -> VALUE 栈装箱（实参 typed、形参动态 NONE 时绑定用） ===== */
    OPC_BOX_INT64,      // INT64 栈弹1 -> lumyr_make_int64 -> VALUE 栈
    OPC_BOX_DOUBLE,     // DOUBLE 栈弹1 -> lumyr_make_double -> VALUE 栈
    OPC_BOX_PTR,        // PTR 栈弹1，a=CastKind -> 对应 Value -> VALUE 栈

    // 多 catch 类型匹配：a=异常类型字符串常量下标(-1=捕获全部)，b=不匹配跳转pc
    OPC_CATCH_MATCH,

    // null 字面量：压入 VALUE 栈的 NONE
    OPC_PUSH_NONE,

    // VALUE 栈 -> typed 栈拆箱（赋值给已 typed 变量时使用）
    OPC_UNBOX_INT64,    // VALUE 栈弹1 -> 取 i64 -> INT64 栈
    OPC_UNBOX_DOUBLE,   // VALUE 栈弹1 -> 取 double -> DOUBLE 栈
    OPC_UNBOX_PTR,      // VALUE 栈弹1 -> 取裸指针/字符串复制 -> PTR 栈

    // 上下文目标 VALUE：直接构造 Value 压 VALUE 栈（省去 typed push + BOX）
    OPC_PUSH_INT_VAL,   // a=小 int32 -> make_int64 -> VALUE 栈
    OPC_PUSH_CONST_VAL, // a=常量池下标 -> 按常量类型构造 Value -> VALUE 栈

    // 类型化数组下标写入：VALUE 栈弹 val、idx、arr，按 arr.elem_type 转换写入
    OPC_TYPED_INDEX_SET,

    // 内置方法调用（方法/属性形式）：VALUE 栈弹 b 个实参再弹 receiver；
    // a=BuiltinId。与 OPC_BUILTIN（全局形式）共用同一 builtin_dispatch 实现
    OPC_CALL_BUILTIN_METHOD,

    // PTR 栈字符串 -> 数值解析（三元分支统一/显式转型路径）
    OPC_STR_TO_INT64,   // PTR 栈弹 char* -> strtoll -> INT64 栈（不 free，同 FROM_STRING 惯例）
    OPC_STR_TO_DOUBLE,  // PTR 栈弹 char* -> strtod  -> DOUBLE 栈
} OpCode;

/* ============================================================
 * Instruction 结构体：三地址格式字节码指令
 * a / b 的具体含义由 op 决定，常见用法：
 *   a: 符号表下标 / 常量池下标 / 直接常量值 / 内置函数ID / 跳转目标PC / 64位常量低32位
 *   b: 元素个数 / 参数个数 / 64位常量高32位 / 辅助参数
 * 无操作数的指令 a、b 均为 0
 * ============================================================ */
typedef struct {
    OpCode op;
    int a;
    int b;
} Instruction;

/* ============================================================
 * BuiltinId 枚举：内置函数 ID（OPC_BUILTIN 的 a 字段）
 * ============================================================ */
typedef enum {
    BUILTIN_LEN = 0,      // len(x)：数组/字符串长度
    BUILTIN_TYPE,         // type(x)：类型名
    BUILTIN_INPUT,        // input()：读一行
    BUILTIN_RANGE,        // range(n)：[0..n-1] 数组
    BUILTIN_SUBSTR,       // substr(s, start, n)
    BUILTIN_TOUPPER,      // toupper(s)
    BUILTIN_TOLOWER,      // tolower(s)
    BUILTIN_SPLIT,        // split(s, sep)
    BUILTIN_DEL,          // del(arr, idx)
    BUILTIN_INSERT,       // insert(arr, idx, val)
    BUILTIN_FLOOR,        // floor(x)
    BUILTIN_CEIL,         // ceil(x)
    BUILTIN_ABS,          // abs(x)
    BUILTIN_SQRT,         // sqrt(x)
    BUILTIN_MAX,          // max(a, b, ...) 变参
    BUILTIN_MIN,          // min(a, b, ...) 变参
    BUILTIN_JOIN,         // join(arr, sep)
    BUILTIN_CONTAINS,     // contains(s/arr, x)
    BUILTIN_REPEAT,       // repeat(s, n)
    BUILTIN_REPLACE,      // replace(s, from, to)
    BUILTIN_SUM,          // sum(arr)
    BUILTIN_AVG,          // avg(arr)
    BUILTIN_FORMAT,       // format(fmt, args...) 变参
    BUILTIN_SORT,         // sort(arr)
    BUILTIN_REVERSE,      // reverse(arr)
    BUILTIN_MAP,          // map(arr, fn) 高阶
    BUILTIN_FILTER,       // filter(arr, fn) 高阶
    BUILTIN_REDUCE,       // reduce(arr, fn, init) 高阶
    BUILTIN_STRIP,        // strip(s)
    BUILTIN_STARTSWITH,   // startswith(s, prefix)
    BUILTIN_ENDSWITH,     // endswith(s, suffix)
    BUILTIN_READ_FILE,    // read_file(path) → 文件内容
    BUILTIN_WRITE_FILE,   // write_file(path, content)
    BUILTIN_FILE_EXISTS,  // file_exists(path) → bool
    BUILTIN_KEYS,         // keys(d) → 键字符串数组
    BUILTIN_VALUES,       // values(d) → 值数组
    BUILTIN_THREAD,       // thread(f, args...) → 线程id（多线程）
    BUILTIN_THREAD_JOIN,  // thread_join(tid) → 等待线程并取返回值（join 已被字符串拼接占用）
    BUILTIN_MUTEX,        // mutex() → 互斥锁 id
    BUILTIN_RMUTEX,       // rmutex() → 递归互斥锁 id
    BUILTIN_RWLOCK,       // rwlock() → 读写锁 id
    BUILTIN_SPINLOCK,     // spinlock() → 自旋锁 id
    BUILTIN_LOCK,         // lock(id) → 阻塞加锁
    BUILTIN_UNLOCK,       // unlock(id) → 解锁
    BUILTIN_TRYLOCK,      // trylock(id) → bool（非阻塞尝试）
    BUILTIN_RDLOCK,       // rdlock(id) → 读锁（读写锁）
    BUILTIN_WRLOCK,       // wrlock(id) → 写锁（读写锁）
    BUILTIN_TRYRDLOCK,    // tryrdlock(id) → bool（读锁非阻塞尝试，仅读写锁）
    BUILTIN_TRYWRLOCK,    // trywrlock(id) → bool（写锁非阻塞尝试，仅读写锁）
    BUILTIN_CONDVAR,      // condvar() → 条件变量 id
    BUILTIN_COND_WAIT,    // cond_wait(cond, lock) → 原子释放锁并等待
    BUILTIN_COND_TIMEDWAIT, // cond_wait_timeout(cond, lock, ms) → bool（唤醒 true / 超时 false）
    BUILTIN_COND_SIGNAL,  // cond_signal(cond) → 唤醒一个等待者
    BUILTIN_COND_BROADCAST, // cond_broadcast(cond) → 唤醒全部等待者
    BUILTIN_THREADLOCAL_GET, // threadlocal_get(name) → 当前线程局部值
    BUILTIN_THREADLOCAL_SET, // threadlocal_set(name, value) → 写当前线程局部槽，返回 value
    BUILTIN_HTTP_GET,     // requests.get(url, params?, config?) → map{status,body,headers}
    BUILTIN_HTTP_POST,    // requests.post(url, params?, config?)
    BUILTIN_HTTP_PUT,     // requests.put(url, params?, config?)
    BUILTIN_HTTP_DELETE,  // requests.delete(url, params?, config?)
    BUILTIN_HTTP_HEAD,    // requests.head(url, params?, config?)
    BUILTIN_HTTP_PATCH,   // requests.patch(url, params?, config?)
    BUILTIN_JSON,         // json(s)：解析 JSON 文本 → 值
    BUILTIN_STRINGIFY,    // stringify(v)：值 → JSON 文本
    BUILTIN_ARRAY_ADD,    // add(arr, x)：末尾追加，原地修改并返回 self（arr.add(x) 链式）
    BUILTIN_ARRAY_REMOVE, // remove(arr, i)：删下标 i 元素，原地修改并返回 self
    BUILTIN_ARRAY_CLEAR,  // clear(arr)：清空，原地修改并返回 self
    BUILTIN_ARRAY_INDEXOF,// indexOf(arr, x)：按值相等查首个下标，-1 未找到
    BUILTIN_GET,          // get(arr/map, i/key)：安全取元素/键值（越界/缺键 → null）
    BUILTIN_SET,          // set(arr, i, v) / set(m, k, v)：原地写并返回 self（链式）
    BUILTIN_ARRAY_FIRST,  // first(arr)：首元素（空 → null）
    BUILTIN_ARRAY_LAST,   // last(arr)：尾元素（空 → null）
    BUILTIN_MAP_HAS,      // has(m, k)：键是否存在（m.has(k) 方法链）
    BUILTIN_ARRAY_FLAT,   // flat(arr, depth?)：数组/字典扁平化（.flat() 方法链）
    BUILTIN_QS,           // qs(v)：字典/数组 → 查询字符串；字符串 → 解析为字典/数组
    BUILTIN_ARRAY_ADDALL, // addAll(a, b)：数组追加全部元素 / 字典合并全部键值
    BUILTIN_BYTES,        // bytes(s, enc?)：字符串 → 字节数组（按编码，默认 UTF-8）
    BUILTIN_STR,          // str(arr, enc?)：字节数组 → 字符串（按编码，默认 UTF-8）
    BUILTIN_ENCODE,       // encode(s, enc?)：字符串 → 字节数组（按编码，默认 UTF-8）
    BUILTIN_DECODE,       // decode(arr, enc?)：字节数组 → 字符串（按编码，默认 UTF-8）
    BUILTIN_ENCODE_URL,   // encodeURL(s)：URL 编码（高字节原样）
    BUILTIN_DECODE_URL,   // decodeURL(s)：URL 解码（%XX/+ → 原字符）
    BUILTIN_MD5,          // md5(s)：MD5 32 位十六进制小写
    BUILTIN_ENCODE_BASE64,  // encodeBase64(s)：Base64 编码
    BUILTIN_DECODE_BASE64,  // decodeBase64(s)：Base64 解码
    BUILTIN_REGEX_MATCH,    // regex_match(s, pattern)：完整匹配 → bool
    BUILTIN_REGEX_SEARCH,   // regex_search(s, pattern)：搜索 → [match, group1, ...]
    BUILTIN_REGEX_REPLACE,  // regex_replace(s, pattern, repl)：替换所有匹配（支持 \1 反向引用）
    BUILTIN_NOW,            // now()：当前时间 map
    BUILTIN_TIMESTAMP,      // timestamp()：Unix 秒（double）
    BUILTIN_TIMESTAMP_MS,   // timestamp_ms()：Unix 毫秒（int）
    BUILTIN_SLEEP,          // sleep(ms)：休眠毫秒
    BUILTIN_DATE,           // date()："2026-09-07"
    BUILTIN_TIME,           // time()："15:30:45"
    BUILTIN_DATETIME,       // datetime()："2026-09-07 15:30:45"
    BUILTIN_FORMAT_TIME,    // format_time(fmt, ts?)：strftime 格式化
    BUILTIN_LOG_DEBUG,      // debug(msg) / log.debug(msg)
    BUILTIN_LOG_INFO,       // info(msg) / log.info(msg)
    BUILTIN_LOG_WARN,       // warn(msg) / log.warn(msg)
    BUILTIN_LOG_ERROR,      // error(msg) / log.error(msg)
    BUILTIN_LOG_FATAL,      // fatal(msg) / log.fatal(msg)
    BUILTIN_GC_COUNT,       // gc_count()：当前 GC 管理对象数
    BUILTIN_GC_BYTES,       // gc_bytes()：当前 GC 管理字节数（近似）
    BUILTIN_GC_COLLECT,     // gc_collect()：手动触发一次 GC
    BUILTIN_GC_STW_NS,      // gc_stw_ns()：累计 STW 停顿时间（纳秒）
    BUILTIN_NEXT,            // next(gen)：恢复生成器执行，返回 yield 值；结束返回 null
    BUILTIN_SEND,            // send(gen, val)：向生成器发送值，返回下一个 yield 值
    BUILTIN_RECEIVE,         // receive()：在生成器中获取 send() 发送的值
    BUILTIN_CLOSE,           // close(gen)：关闭生成器
    BUILTIN_GEN_THROW,       // GenThrow(gen, err)：向生成器抛出异常，在 yield 位置抛出
    BUILTIN_CHAIN,           // chain(g1, g2)：连接两个生成器
    BUILTIN_ZIP,             // zip(g1, g2)：压缩两个生成器
    BUILTIN_SKIP,            // skip(g, n)：跳过前 n 个元素
    BUILTIN_TAKE,            // take(g, n)：取前 n 个元素
    BUILTIN_ENUMERATE,       // enumerate(g)：枚举 [index, value]

    /* ===== 数组补充（方法/全局两种形式，语义见方法×类型矩阵） ===== */
    BUILTIN_OBJECT_INDEX,    // objectIndex(arr, obj)：按对象引用（指针身份）查下标，-1 未找到
    BUILTIN_CHAR_AT,         // char_at(s, i)：取第 i 个字符（1 字符串）

    /* ===== AI / 线性代数（嵌套数组矩阵 + TypedArray 向量，热路径零装箱） ===== */
    BUILTIN_SHAPE,           // shape(arr)：各维长度 int[]（不规则报错）
    BUILTIN_RESHAPE,         // reshape(arr, d0, d1, ...)：按新形状重排，返回新数组
    BUILTIN_SLICE,           // slice(arr, start, end?)：顶层切片 [start, end)，返回新数组
    BUILTIN_CONCAT,          // concat(a, b)：顶层拼接，返回新数组
    BUILTIN_DOT,             // dot(a, b)：1D 点积 → 标量
    BUILTIN_MATMUL,          // matmul(A, B)：2D×2D 矩阵乘 → 新嵌套数组
    BUILTIN_TRANSPOSE,       // transpose(A)：2D 转置 → 新嵌套数组
    BUILTIN_NORM,            // norm(v)：L2 范数 → 标量
    BUILTIN_DET,             // det(A)：2D 方阵行列式（高斯消元，double）→ 标量
    BUILTIN_INV,             // inv(A)：2D 方阵求逆（高斯消元）→ 新嵌套数组，行类型跟随输入
    BUILTIN_MEAN,            // mean(v)：算术平均 → 标量（= avg 别名实现共用）
    BUILTIN_STD,             // std(v)：总体标准差 → 标量
    BUILTIN_VARIANCE,        // var(v)：总体方差 → 标量
    BUILTIN_ARGMAX,          // argmax(v)：最大值下标，-1 空数组
    BUILTIN_ARGMIN,          // argmin(v)：最小值下标，-1 空数组
    BUILTIN_NORMALIZE,       // normalize(v)：L2 归一化 → 新数组/同型 TypedArray
    BUILTIN_SOFTMAX,         // softmax(v)：softmax → 新数组/同型 TypedArray

    BUILTIN_COUNT
} BuiltinId;

/* ============================================================
 * 统一常量池条目
 * 小常量（int32 范围）直接内嵌在指令里，不进池
 * 大常量（int64/uint64/double/string）进池，指令存索引
 * ============================================================ */
typedef enum {
    CONST_INT64,    // 有符号 64 位整数
    CONST_UINT64,   // 无符号 64 位整数
    CONST_DOUBLE,   // 双精度浮点数
    CONST_STRING    // 字符串指针
} ConstType;

typedef struct {
    ConstType type;
    union {
        int64_t i64;
        uint64_t u64;
        double d;
        const char* s;
    };
} ConstEntry;

/* ============================================================
 * CallSite：一次用户函数调用点的静态信息
 * OPC_CALL 的 a 字段 = 该 CallSite 在所属 BytecodeFunc.callsites 中的下标
 * ============================================================ */
typedef struct CallSite {
    char* callee;        /* 被调用函数名（VM 据此查函数表） */
    int argc;            /* 绑定到形参的实参个数（含默认值补全） */
    int* arg_is_ref;     /* 每个形参是否按 ref 传递（长度 argc） */
    int* arg_ref_slots;  /* ref 实参在调用方帧的槽位（非 ref 为 -1，长度 argc） */
    int keep_result;     /* 1=压返回值（表达式语境）；0=丢弃（表达式语句语境） */
    int ret_stack;       /* 返回值压入的栈（ExprType：INT/DOUBLE/PTR/NONE→VALUE） */
} CallSite;

/* ============================================================
 * SymHash：符号名 → 数组下标的开放寻址哈希索引（编译期使用）
 * 替代线性数组扫描：符号数 N 很大时，bf_sym/c_find_var 由 O(N) 降为 O(1)，
 * 消除大文件编译的 O(N^2) strcmp 开销。tab 存下标，-1 空槽；cap 为 2 的幂。
 * ============================================================ */
typedef struct {
    int* tab;
    int  cap;
} SymHash;

/* ============================================================
 * BytecodeFunc 结构体：一个可执行单元（main 或一个 lum 函数）
 * ============================================================ */
typedef struct {
    const char* name;          // 函数名（main 为 NULL）
    int is_main;
    Instruction* code;
    int code_len, code_cap;
    char** syms;               // 符号名池（变量名/函数名）
    int sym_cnt, sym_cap;
    ConstEntry* const_pool;    // 统一常量池（大常量：int64/uint64/double/string）
    int const_cnt, const_cap;
    char** params;             // 参数名（普通参数在前，可变参数最后）
    int param_cnt;             // 普通参数个数
    int has_variadic;
    int* param_is_ref;         // 参数是否是 ref 引用传递（1=ref，0=值传递），长度 param_cnt
    int is_generator;          // 是否为生成器函数（gen func）
    int* var_type_tags;        // 变量类型标记（CastKind 枚举，-1 表示无标记），与 syms 平行数组
    char** var_struct_names;    // 变量的 struct 类型名（NULL 表示不是 struct），与 syms 平行数组
    int is_method;              // 是否为结构体方法（self 参数传递指针）
    char* method_self_struct;   // 方法 self 参数的 struct 类型名
    char* class_name;           // 方法所属的 class 名（NULL 表示不是 class 方法）
    char* ret_type_name;        // 返回值类型名（如 "int"/"double"，NULL=无标注）
    CallSite* callsites;        // 本函数内全部调用点（OPC_CALL.a 索引）
    int callsite_cnt, callsite_cap;
    SymHash sym_idx;            // syms 符号名 → 下标哈希（编译期 O(1) 查找）
} BytecodeFunc;

#endif // LUMYR_IR_BYTECODE_TYPE_H
