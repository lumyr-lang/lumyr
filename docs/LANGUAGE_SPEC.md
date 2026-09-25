# Lumyr 语言规范

## 1. 概述

Lumyr 是一门极简的编译型编程语言，使用 Flex/Bison 实现，将源码转译为 C 并编译为原生二进制。支持 VM 解释执行和编译执行两种模式。

## 2. 数据类型

| 类型 | 说明 | 示例 |
|------|------|------|
| `int` | 64位整数 | `42`, `-10` |
| `double` | 双精度浮点数 | `3.14`, `2.5` |
| `bool` | 布尔值 | `true`, `false` |
| `char` | 字符 | `'a'`, `'Z'` |
| `string` | 字符串 | `"hello"`, `f"hi {name}"` |
| `array` | 数组 | `[1, 2, 3]`, `[]` |
| `map` | 映射/字典 | `{"a": 1, "b": 2}` |
| `func` | 函数 | `func f(x) { return x; }` |
| `null` | 空值 | `null` |
| `byte` | 8位无符号整数 | `0-255` |

## 3. 变量与赋值

```lumyr
// 变量声明与赋值（动态类型）
x = 42;
x = "hello";  // 可以重新赋值不同类型

// 复合赋值
x += 5;
x -= 3;
x *= 2;
x /= 2;

// 自增自减
i++;
i--;
```

## 4. 运算符

### 算术运算符
| 运算符 | 说明 | 示例 |
|--------|------|------|
| `+` | 加法/字符串拼接 | `a + b` |
| `-` | 减法 | `a - b` |
| `*` | 乘法 | `a * b` |
| `/` | 除法 | `a / b` |
| `%` | 取模 | `a % b` |

### 比较运算符
| 运算符 | 说明 | 示例 |
|--------|------|------|
| `==` | 等于 | `a == b` |
| `!=` | 不等于 | `a != b` |
| `<` | 小于 | `a < b` |
| `>` | 大于 | `a > b` |
| `<=` | 小于等于 | `a <= b` |
| `>=` | 大于等于 | `a >= b` |

### 逻辑运算符
| 运算符 | 说明 | 示例 |
|--------|------|------|
| `&&` | 逻辑与 | `a && b` |
| `\|\|` | 逻辑或 | `a \|\| b` |
| `!` | 逻辑非 | `!a` |

### 其他运算符
| 运算符 | 说明 | 示例 |
|--------|------|------|
| `?:` | 三元运算符 | `a > b ? a : b` |
| `?.` | 安全调用 | `obj?.method()` |
| `??` | 空合并 | `a ?? default` |
| `[]` | 下标访问 | `arr[0]`, `map["key"]` |
| `.` | 成员访问 | `obj.method()` |

## 5. 控制流

### if/else if/else
```lumyr
if (x > 10) {
    print("big");
} else if (x > 5) {
    print("medium");
} else {
    print("small");
}
```

### while 循环
```lumyr
i = 0;
while (i < 10) {
    print(i);
    i++;
}
```

### do-while 循环
```lumyr
i = 0;
do {
    print(i);
    i++;
} while (i < 10);
```

### for 循环（C 风格）
```lumyr
for (i = 0; i < 10; i++) {
    print(i);
}
```

### for-each 循环
```lumyr
// 遍历数组
for x in arr {
    print(x);
}

// 遍历映射
for k, v in map {
    print(k + " = " + v);
}
```

### switch/match 语句
```lumyr
// switch 语句（支持 fallthrough）
switch (x) {
    case 1:
        print("one");
        break;
    case 2:
        print("two");
        break;
    default:
        print("other");
}

// match 语句（switch 的别名）
match (x) {
    case 1:
        print("one");
        break;
    default:
        print("other");
}

// 类型匹配
match (x) {
    case int:
        print("integer");
        break;
    case string:
        print("string");
        break;
    case double:
        print("double");
        break;
    case bool:
        print("boolean");
        break;
    default:
        print("other");
}
```

### break / continue
```lumyr
for (i = 0; i < 10; i++) {
    if (i == 5) break;      // 跳出循环
    if (i % 2 == 0) continue; // 跳过本次迭代
    print(i);
}
```

### return
```lumyr
func add(a, b) {
    return a + b;
}
```

## 6. 函数

### 函数定义
```lumyr
func greet(name) {
    print("Hello, " + name);
}
```

### 默认参数
```lumyr
func power(base, exp=2) {
    result = 1;
    for (i = 0; i < exp; i++) {
        result = result * base;
    }
    return result;
}

power(3);      // 9 (3^2)
power(3, 3);   // 27 (3^3)
```

### 编译期计算函数（const func）
```lumyr
const func square(x) {
    return x * x;
}

print(square(5));  // 25，编译期计算
```

### 宏（macro）
```lumyr
macro print_square(x) {
    print(x * x);
}

print_square(7);  // 49，编译期展开
```

### 注解（annotation）
```lumyr
@deprecated
func old_func() {
    return "old";
}
```

### 闭包 / lambda
```lumyr
f = func(x) { return x * 2; };
print(f(5));  // 10
```

## 7. 异常处理

### try/catch/finally
```lumyr
try {
    // 可能抛出异常的代码
    throw "Something went wrong";
} catch (e) {
    print("Caught: " + e);
} finally {
    print("Always executed");
}
```

### throw
```lumyr
func divide(a, b) {
    if (b == 0) {
        throw "Division by zero";
    }
    return a / b;
}
```

## 8. 空安全

```lumyr
// 安全调用 ?.
result = obj?.method();  // 如果 obj 为 null，返回 null

// 空合并 ??
value = possibly_null ?? default_value;

// 手动空检查
func get_length(s) {
    if (s == null) {
        return 0;
    }
    return len(s);
}
```

## 9. 字符串

### 字符串字面量
```lumyr
s = "Hello, World";
fstring = f"Hello, {name}";  // 格式化字符串
```

### 字符串操作
```lumyr
len(s);              // 长度
substr(s, 0, 5);     // 子串
toupper(s);          // 转大写
tolower(s);          // 转小写
contains(s, "World"); // 包含
startswith(s, "Hello"); // 开头
endswith(s, "World");   // 结尾
strip("  pad  ");       // 去空白
split("a,b,c", ",");    // 分割
join(["a","b","c"], "-"); // 连接
repeat("ab", 3);        // 重复
replace("banana", "na", "NA"); // 替换
format("{} + {} = {}", 1, 2, 3); // 格式化
```

## 10. 数组

### 数组字面量
```lumyr
arr = [1, 2, 3, 4, 5];
empty = [];
```

### 数组操作
```lumyr
len(arr);           // 长度
arr[0];              // 访问元素
arr[0] = 10;         // 修改元素
arr_add(arr, 6);     // 添加元素
arr_insert(arr, 0, 0); // 插入元素
arr_remove(arr, 0);  // 删除元素
arr_clear(arr);      // 清空
array_sort(arr);     // 排序
array_reverse(arr);  // 反转
```

## 11. 映射（Map）

### 映射字面量
```lumyr
m = {"a": 1, "b": 2, "c": 3};
empty = {};
```

### 映射操作
```lumyr
m["a"];              // 访问值
m["d"] = 4;           // 设置值
map_get(m, "a");      // 获取值
map_set(m, "a", 10);  // 设置值
map_has(m, "a");      // 是否包含键
map_del(m, "a");      // 删除键
map_keys(m);          // 所有键
map_values(m);        // 所有值
len(m);               // 键值对数量
```

## 12. 内置函数

### 类型与转换
| 函数 | 说明 | 示例 |
|------|------|------|
| `type(x)` | 返回值的类型名 | `type(42)` → `"int"` |
| `int(x)` | 转换为整数 | `int("42")` → `42` |
| `double(x)` | 转换为浮点数 | `double("3.14")` → `3.14` |
| `string(x)` | 转换为字符串 | `string(42)` → `"42"` |
| `bool(x)` | 转换为布尔值 | `bool(0)` → `false` |
| `char(x)` | 转换为字符 | `char(65)` → `'A'` |

### 数学函数
| 函数 | 说明 | 示例 |
|------|------|------|
| `abs(x)` | 绝对值 | `abs(-5)` → `5` |
| `floor(x)` | 向下取整 | `floor(3.7)` → `3` |
| `ceil(x)` | 向上取整 | `ceil(3.2)` → `4` |
| `round(x)` | 四舍五入 | `round(3.5)` → `4` |
| `sqrt(x)` | 平方根 | `sqrt(16)` → `4` |
| `pow(x, y)` | 幂运算 | `pow(2, 3)` → `8` |
| `min(a, b)` | 最小值 | `min(3, 5)` → `3` |
| `max(a, b)` | 最大值 | `max(3, 5)` → `5` |
| `rand()` | 随机数 | `rand()` → `0-1` |

### 输入输出
| 函数 | 说明 | 示例 |
|------|------|------|
| `print(x)` | 打印并换行 | `print("hello")` |
| `input()` | 读取一行输入 | `name = input()` |
| `printf(fmt, ...)` | 格式化打印 | `printf("%d", 42)` |

### JSON
| 函数 | 说明 | 示例 |
|------|------|------|
| `json_parse(s)` | 解析 JSON 字符串 | `json_parse('{"a":1}')` |
| `json_stringify(obj)` | 序列化为 JSON 字符串 | `json_stringify({"a":1})` |

### 正则表达式
| 函数 | 说明 | 示例 |
|------|------|------|
| `regex_match(s, pattern)` | 正则匹配 | `regex_match("abc", "a.*")` |
| `regex_replace(s, pattern, repl)` | 正则替换 | `regex_replace("abc", "a", "A")` |

### 时间
| 函数 | 说明 | 示例 |
|------|------|------|
| `time()` | 当前时间戳 | `time()` |
| `sleep(ms)` | 休眠毫秒 | `sleep(1000)` |
| `clock()` | 程序运行时间 | `clock()` |

### 垃圾回收
| 函数 | 说明 | 示例 |
|------|------|------|
| `gc_collect()` | 强制垃圾回收 | `gc_collect()` |
| `gc_count()` | GC 对象数量 | `gc_count()` |
| `gc_bytes()` | GC 使用字节数 | `gc_bytes()` |

## 13. 模块系统

```lumyr
// 导入模块
import "module.lm";

// 导入并使用
import "math.lm" as math;
math.sqrt(16);
```

## 14. 线程（可选）

```lumyr
// 创建线程
t = thread(func() {
    print("in thread");
});

// 等待线程结束
thread_join(t);

// 锁
lock = lock_create();
lock_acquire(lock);
// 临界区
lock_release(lock);
```

## 15. 注释

```lumyr
// 单行注释

/*
 * 多行注释
 */
```

## 16. 程序入口

Lumyr 程序不需要显式的 `main` 函数，顶层代码会按顺序执行。函数定义不会执行，只有调用时才执行。

```lumyr
// 顶层代码会立即执行
print("Hello, World!");

// 函数定义不会执行，只有调用时才执行
func main() {
    print("in main");
}

main();  // 调用函数
```
