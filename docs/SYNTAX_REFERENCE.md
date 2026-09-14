# Lumyr 语言语法参考文档

> 本文档记录 Lumyr 语言的所有语法规则，基于 `src/parse/yacc.y` 和测试文件中的实际用法整理。
> **重要：尖括号 `<>` 是类型声明/标注，括号 `()` 是类型转换。**

---

## 1. 基本语法

### 1.1 语句结束
- 所有语句必须以分号 `;` 结束
- 块语句 `{ ... }` 后面不需要分号

```lumyr
print("hello");
x = 42;
```

### 1.2 注释
```lumyr
// 单行注释
/* 多行注释 */
```

### 1.3 变量声明
- 不需要 `let`/`var` 关键字，直接赋值
- 类型标注可选，用尖括号 `<>`

```lumyr
x = 42;              // 默认 long long
y = <int>42;         // 精确类型 int
z = <double>3.14;    // 精确类型 double
s = "hello";         // 默认 string
```

---

## 2. 类型系统

### 2.1 类型声明（尖括号 `<>`）
**尖括号 `<>` 用于类型声明/标注，不是类型转换。**

#### 变量类型标注
```lumyr
a = <int>8;           // 等价于 C: int a = 8;
b = <long long>8;     // 等价于 C: long long b = 8;
c = <double>3.14;     // 等价于 C: double c = 3.14;
d = <float>3.14;      // 等价于 C: float d = 3.14;
e = <char>'a';         // 等价于 C: char e = 'a';
f = <bool>true;        // 等价于 C: bool f = true;
```

#### 函数参数类型标注
```lumyr
func add(<int> a, <int> b) {
    return a + b;
}

func process(<Point> p) {
    // p 是 Point 类型
}
```

#### ref 参数类型标注
```lumyr
func modify(ref <Point> p) {
    p.x = 999;  // 修改会影响外部
}
```

#### 函数返回值类型标注
```lumyr
func add(<int> a, <int> b): <int> {
    return a + b;
}
```

### 2.2 类型转换（括号 `()`）
**括号 `()` 用于类型转换（强转）。**

```lumyr
x = (int)3.14;      // double 转 int
y = (double)42;     // int 转 double
z = (string)42;     // int 转 string
```

### 2.3 支持的基本类型

#### 通用类型
| 类型 | 说明 | C 对应类型 |
|------|------|-----------|
| `int` | 整数 | `int` |
| `short` | 短整数 | `short` |
| `long` | 长整数 | `long` |
| `long long` | 长长整数 | `long long` |
| `uint` | 无符号整数 | `unsigned int` |
| `unsigned short` | 无符号短整数 | `unsigned short` |
| `unsigned long` | 无符号长整数 | `unsigned long` |
| `unsigned char` | 无符号字符 | `unsigned char` |
| `float` | 单精度浮点数 | `float` |
| `double` | 双精度浮点数 | `double` |
| `long double` | 长双精度浮点数 | `long double` |
| `char` | 字符 | `char` |
| `ascii` | ASCII 字符 | `char` |
| `bool` | 布尔值 | `bool` |
| `string` | 字符串 | `char*` |
| `byte` | 字节 | `unsigned char` |
| `void` | 空类型 | `void` |
| `size_t` | 大小类型 | `size_t` |
| `ssize_t` | 有符号大小类型 | `ssize_t` |

#### 固定宽度整数类型
| 类型 | 说明 | C 对应类型 |
|------|------|-----------|
| `int8` | 8 位有符号整数 | `int8_t` |
| `int16` | 16 位有符号整数 | `int16_t` |
| `int32` | 32 位有符号整数 | `int32_t` |
| `int64` | 64 位有符号整数 | `int64_t` |
| `uint8` | 8 位无符号整数 | `uint8_t` |
| `uint16` | 16 位无符号整数 | `uint16_t` |
| `uint32` | 32 位无符号整数 | `uint32_t` |
| `uint64` | 64 位无符号整数 | `uint64_t` |

#### FFI 特殊类型
| 类型 | 说明 |
|------|------|
| `ptr` | 通用指针 |
| `pointer` | 指针（同 ptr） |
| `handle` | 句柄（同 ptr） |

### 2.4 类型标注的使用场景

类型标注（尖括号 `<>`）可以用于以下场景：

#### 变量声明
```lumyr
a = <int>8;           // 等价于 C: int a = 8;
b = <double>3.14;     // 等价于 C: double b = 3.14;
c = <int8>1000;       // 截断为 int8: -24
d = <uint8>(-1);      // 转换为 uint8: 255
```

#### 非常量表达式
```lumyr
x = 1000;
print(<int8>x);        // -24
print(<uint16>(x * 2)); // 1000
```

#### 复合表达式
```lumyr
print(<int8>(100 + 900));  // -24 (先计算再截断)
print(<int8>100 + 900);    // 1000 (只截断 100，再加 900)
```

#### 函数返回值
```lumyr
func get_val() { return 1000; }
print(<int8>get_val());  // -24
```

### 2.5 数组泛型类型标注

```lumyr
// 基本类型数组
ai = <int>[1, 2, 3];
ad = <double>[1.1, 2.2];
ab = <bool>[true, false, 1];
ac = <char>['a', 'b', 99];
as_ = <string>["x", "y", 123];
aby = <byte>[200, 256, -1];

// 固定宽度整数数组
ai8 = <int8>[200, 127, 128];
au8 = <uint8>[200, 256, -1];
ai32 = <int32>[2147483648, 100];

// 空数组
e1 = <int>[];
e2 = <string>[];
e3 = <uint8>[];
e4 = <long long>[];
```

### 2.6 Map 泛型类型标注

```lumyr
// 基本值类型 Map
mi = <string,int>{"a": 1, "b": 2};
md = <string,double>{"x": 1.5};
mb = <string,bool>{"t": true, "f": 0};
mc = <string,char>{"k": 'A', "n": 66};
ms = <string,string>{"a": "1", "b": 2};
mby = <string,byte>{"a": 200, "b": 256};

// 固定宽度整数 Map
mi8 = <string,int8>{"a": 200, "b": 127};
mu8 = <string,uint8>{"a": 200, "b": -1};
mi32 = <string,int32>{"big": 2147483648};
```

### 2.7 默认类型
- 整数默认 `long long`
- 浮点数默认 `double`
- 没有类型标注时，Value 默认给上 `long long` 的类型标记

---

## 3. 函数定义

### 3.1 基本函数定义
```lumyr
func name(params) {
    // 函数体
}
```

### 3.2 带返回值类型标注
```lumyr
func add(<int> a, <int> b): <int> {
    return a + b;
}
```

### 3.3 默认参数
```lumyr
func greet(name, greeting = "Hello") {
    print(greeting, name);
}
```

### 3.4 可变参数
```lumyr
func sum(...args) {
    total = 0;
    for x in args {
        total = total + x;
    }
    return total;
}
```

### 3.5 ref 引用传递参数
```lumyr
func modify(ref <Point> p) {
    p.x = 999;  // 修改会影响外部
}
```

### 3.6 生成器函数
```lumyr
gen func countdown(n) {
    while (n > 0) {
        yield n;
        n = n - 1;
    }
}
```

### 3.7 编译期常量函数
```lumyr
const func square(x) {
    return x * x;
}
```

### 3.8 泛型函数
```lumyr
func<T> identity(x: T) {
    return x;
}
```

### 3.9 运算符重载
```lumyr
func +(other) {
    return Point(x + other.x, y + other.y);
}
```

### 3.10 FFI 外部函数声明
```lumyr
extern <int> func printf(<string> format, ...);
extern "libcurl" <int> func curl_easy_init();
extern "msvcrt.dll" <ptr> func strchr(<string>s, <int>c);
extern "msvcrt.dll" <void> func srand(<int>seed);
extern "msvcrt.dll" <handle> func malloc(<int>size);
```

### 3.11 Lambda 匿名函数

Lambda 是匿名函数，可以直接赋值给变量或作为参数传递。

```lumyr
// 基本 lambda
g = func(x) { return x * 2 + 5; };
print(g(5));  // 15

// 多参数 lambda
add_fn = func(a, b) { return a + b; };
print(add_fn(3, 4));  // 7

// 作为高阶参数传递
arr = [1, 2, 3, 4, 5];
print(join(map(arr, func(x) { return x * x; }), ","));   // 1,4,9,16,25
print(join(filter(arr, func(x) { return x % 2 == 0; }), ","));  // 2,4
print(reduce(arr, func(acc, v) { return acc + v; }, 0));  // 15

// lambda 访问全局变量
g_multiplier = 3;
mul_global = func(x) { return x * g_multiplier; };
print(mul_global(7));  // 21
```

**注意**：Lumyr 的 lambda 是"无闭包"设计——编译期强制禁止 lambda 体访问外层函数局部变量，只能访问：自身参数、全局变量、以及 lambda 体内赋值产生的局部变量。这从根本上杜绝了 UAF 和悬垂引用。

---

## 4. 结构体（struct）

### 4.1 struct 定义
```lumyr
struct Point {
    x: int,
    y: int,
    func distance(other: Point): <double> {
        return (x - other.x)^2 + (y - other.y)^2;
    }
}
```

### 4.2 struct 构造
```lumyr
p = Point(10, 20);
```

### 4.3 struct 字段访问
```lumyr
print(p.x, p.y);
p.x = 100;
```

### 4.4 struct 方法调用
```lumyr
d = p.distance(other);
```

### 4.5 struct 嵌套
```lumyr
struct Rect {
    top_left: Point,
    bottom_right: Point,
}

r = Rect(Point(0, 0), Point(100, 100));
print(r.top_left.x);
```

### 4.6 struct 只读属性
- `__structname__` — struct 类型名（只读）
- `__mapname__` — map 类型名（只读，原 `__classname__` 改名）

---

## 5. 类型别名（type）

### 5.1 type 定义
```lumyr
type Person {
    name: string,
    age: int,
}
```

### 5.2 type 实现接口
```lumyr
type Dog implements Printable {
    name: string,
    func to_string(): <string> {
        return "Dog: " + name;
    }
}
```

### 5.3 泛型 type
```lumyr
type<T> Box {
    value: T,
}
```

---

## 6. 接口（interface）

### 6.1 interface 定义
```lumyr
interface Printable {
    func to_string(): <string>;
}
```

### 6.2 interface 继承
```lumyr
interface Colored extends Printable {
    func get_color(): <string>;
}
```

### 6.3 implements 运算符（类型断言）

运行时判断对象是否实现了某个接口，返回布尔值。

```lumyr
interface Printable {
    func to_string(): <string>;
}

interface Serializable {
    func to_json(): <string>;
}

class Dog implements Printable {
    name: string,
    func to_string(self): <string> {
        return "Dog(" + self.name + ")";
    }
}

class Cat implements Printable, Serializable {
    name: string,
    func to_string(self): <string> { return "Cat"; }
    func to_json(self): <string> { return "{}"; }
}

d = Dog("Buddy");
c = Cat("Whiskers");

print(d implements Printable);      // true
print(d implements Serializable);   // false
print(c implements Printable);      // true
print(c implements Serializable);   // true

// 在 if 语句中使用
if(d implements Printable) {
    print(d.to_string());
}
```

**注意**：
- `implements` 是运算符，不是关键字，用于运行时类型断言
- 支持 VM 模式和 CC 模式（双通道）
- 对于 class 实例，通过 class 注册的接口信息判断
- 对于 type（map）实例，通过 type 注册的接口信息判断

---

## 7. 枚举（enum）

```lumyr
enum Color {
    RED,
    GREEN,
    BLUE,
}
```

---

## 8. 控制流

### 8.1 if/else
```lumyr
if (x > 0) {
    print("positive");
} else if (x < 0) {
    print("negative");
} else {
    print("zero");
}
```

### 8.2 while 循环
```lumyr
while (i < 10) {
    print(i);
    i = i + 1;
}
```

### 8.3 do-while 循环
```lumyr
do {
    print(i);
    i = i + 1;
} while (i < 10);
```

### 8.4 C 风格 for 循环
```lumyr
for (i = 0; i < 10; i = i + 1) {
    print(i);
}

// 省略初始化（变量已在外部定义）
i = 0;
for (; i < 3; i = i + 1) {
    print(i);
}
```

### 8.5 for-each 循环（数组）
```lumyr
for x in arr {
    print(x);
}
```

### 8.6 for-each 循环（map，键值对）
```lumyr
for k, v in mymap {
    print(k, v);
}
```

### 8.7 迭代器协议
```lumyr
for x iter obj {
    print(x);
}
```

### 8.8 switch
```lumyr
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
```

### 8.8.1 match（switch 别名）

`match` 是 `switch` 的别名，语法完全相同，支持字面量匹配和 default 通配符。

```lumyr
// 整数匹配
x = 2;
match (x) {
    case 1:
        print("one");
        break;
    case 2:
        print("two");
        break;
    default:
        print("other");
}

// 字符串匹配
s = "hello";
match (s) {
    case "hello":
        print("greeting");
        break;
    case "bye":
        print("farewell");
        break;
    default:
        print("unknown");
}

// 无 break 的 fallthrough（C 风格）
n = 1;
match (n) {
    case 1:
        print("case1");
    case 2:
        print("case2");
        break;
    default:
        print("default");
}
```

### 8.9 break / continue
```lumyr
break;     // 跳出循环
continue;  // 跳过本次循环
```

### 8.10 return
```lumyr
return value;
return;  // 无返回值
```

---

## 9. 异常处理

### 9.1 try/catch/finally
```lumyr
try {
    // 可能出错的代码
} catch (e) {
    print("Error:", e);
} finally {
    print("always executed");
}
```

### 9.2 throw
```lumyr
throw "error message";
throw Error("something went wrong");
```

---

## 10. 生成器（generator）

### 10.1 生成器定义
```lumyr
gen func range(start, end) {
    i = start;
    while (i < end) {
        yield i;
        i = i + 1;
    }
}
```

### 10.2 生成器使用
```lumyr
g = range(0, 5);
print(next(g));  // 0
print(next(g));  // 1
```

### 10.3 生成器方法
- `.next()` — 获取下一个值
- `.throw(err)` — 向生成器抛出异常
- `.close()` — 关闭生成器

### 10.4 生成器双向通信（send/receive）

生成器支持双向通信：外部通过 `send()` 向生成器发送值，生成器内部通过 `receive()` 接收值。

```lumyr
gen func echo() {
    while (true) {
        val = receive();      // 接收外部发送的值
        yield val;            // 返回接收到的值
    }
}

ge = echo();
r1 = send(ge, "hello");      // 发送 "hello"，receive() 返回 "hello"，yield "hello"
print(r1);                    // hello
r2 = send(ge, 42);           // 发送 42
print(r2);                    // 42
```

### 10.5 生成器组合操作

#### 函数形式
- `chain(g1, g2)` — 连接两个生成器
- `zip(g1, g2)` — 合并两个生成器
- `map(g, fn)` — 映射生成器
- `filter(g, fn)` — 过滤生成器

#### 方法链形式
生成器支持方法链调用，可以链式组合多个操作。

```lumyr
// 跳过前 2 个元素
for x in range1(6).skip(2) { ... }

// 只取前 3 个元素
for x in range1(100).take(3) { ... }

// 连接两个生成器
for x in range1(2).chain(range1(3)) { ... }

// 枚举（带索引）
for x in range1(3).enumerate() { ... }

// 复杂方法链
for x in range1(10).filter(even).map(sq).skip(1).take(2) { ... }
```

---

## 11. 宏（macro）

### 11.1 宏定义
```lumyr
macro DEBUG(msg) {
    print("[DEBUG]", msg);
}
```

### 11.2 宏调用
```lumyr
DEBUG("hello");  // 编译期展开
```

---

## 12. 解构与展开

### 12.1 解构赋值（不需要 unpack 关键字）

直接用逗号分隔的变量列表从数组或函数返回值中解构。

```lumyr
// 数组解构
a, b = [1, 2];
print(a);  // 1
print(b);  // 2

// 多类型解构
x, y, z = ["hello", 3.14, true];

// 函数返回值解构
func mkarr() { return [10, 20, 30]; }
p, q, r = mkarr();
```

### 12.2 unpack 关键字解构

```lumyr
// 对象解构
unpack { x, y } = point;

// 数组解构
unpack [a, b, c] = arr;
```

### 12.3 展开运算符（...）

#### 数组展开
```lumyr
arr = [3, 4];
r1 = [1, 2, ...arr, 5];      // [1, 2, 3, 4, 5]
r2 = [...arr, ...arr];        // [3, 4, 3, 4]
r3 = [...[100, 200], 300];   // [100, 200, 300]
```

#### Map 展开
```lumyr
m1 = {"a": 1, "b": 2};
m2 = {...m1, "c": 3};         // {"a": 1, "b": 2, "c": 3}

// 后面的键覆盖前面的
m3 = {"x": 10};
m4 = {...m3, "x": 20, "y": 30};  // {"x": 20, "y": 30}
```

#### 解构 + 展开组合
```lumyr
data = [1, [2, 3], 4];
first, inner, last = data;
combined = [...inner, first, last];  // [2, 3, 1, 4]
```

---

## 13. 扩展方法（extend）

```lumyr
extend String {
    func shout(): <string> {
        return this + "!!!";
    }
}
```

---

## 14. 注解（annotation）

```lumyr
// 无参数注解
@deprecated
func old_func() {
    // ...
}

// 带参数注解
@deprecated("use new_func instead")
func old_func2() {
    // ...
}

// 其他常用注解
@log
@test
@cached
@optimized
@performance_critical
```

---

## 15. 数据结构

### 15.1 数组
```lumyr
arr = [1, 2, 3, 4, 5];
print(arr[0]);      // 1
arr[0] = 100;
print(len(arr));    // 5
```

### 15.2 Map（字典）
```lumyr
m = {"a": 1, "b": 2};
print(m["a"]);      // 1
m["c"] = 3;
print(keys(m));     // ["a", "b", "c"]
```

#### 多种键类型
Map 的键可以是字符串、整数、字符、浮点数、布尔值等多种类型。

```lumyr
m = {
    "str": 1,       // 字符串键
    42: "int",      // 整数键
    'c': true,      // 字符键
    3.14: "pi",     // 浮点数键
    false: "no"     // 布尔值键
};
print(m["str"]);    // 1
print(m[42]);        // "int"
```

#### 计算键（Computed Keys）
使用 `[expr]` 作为键，键的值在运行时计算。

```lumyr
p1 = "key1";
p2 = "key2";
m = {
    [p1]: {"name": "origin"},
    [p2]: {"name": "far"}
};
print(m["key1"]["name"]);  // origin
```

### 15.3 字符串
```lumyr
s = "hello";
print(s[0]);        // 'h'
print(len(s));      // 5
print(s + " world"); // "hello world"
```

#### 字符串方法
```lumyr
s = "  Hello World  ";

// 常用方法
print(s.len());              // 长度
print(s.strip());            // 去除首尾空白
print(s.toupper());          // 转大写
print(s.tolower());          // 转小写
print(s.contains("World"));  // 是否包含
print(s.replace("World", "Lumyr"));  // 替换
print(s.split(" "));         // 分割

// 子字符串
print(s.strip().substr(0, 5));  // "Hello"（从下标 0 开始，长度 5）

// 编码/解码（UTF-8/GBK）
b = "中文".encode();         // UTF-8 编码
print(b.decode());            // 解码为字符串
bg = "中文".encode("gbk");   // GBK 编码
print(bg.decode("gbk"));     // GBK 解码

// URL 编码/解码
print("a b&c=d".encodeURL());       // a%20b%26c%3Dd
print("a%20b%26c%3Dd".decodeURL()); // a b&c=d

// Base64 编码/解码
print("abc".encodeBase64());        // YWJj
print("YWJj".decodeBase64());       // abc

// 字节转换（支持编码参数）
b = "hello".bytes("big5");          // 字符串转字节（指定编码）
s = str(b, "big5");                  // 字节转字符串（指定编码）
print(str("hello".bytes("big5"), "big5"));  // hello

// 加密哈希
print("hello".md5());        // MD5 哈希
```

#### 字符串函数（函数形式）
```lumyr
// 分割
parts = split("a,b,c", ",");    // ["a", "b", "c"]

// 替换
result = replace("banana", "na", "NA");  // baNANA

// 合并
result = join(["a", "b", "c"], "-");  // "a-b-c"

// 反转
result = reverse([1, 2, 3]);  // [3, 2, 1]

// 排序
result = sort([3, 1, 2]);  // [1, 2, 3]

// 重复
result = repeat("ab", 3);  // "ababab"
```

#### format 函数
```lumyr
// 格式化字符串
print(format("a={}, b={}", 1, 2));  // "a=1, b=2"
print(format("pi={}", 3.14));        // "pi=3.14"
```

#### 正则表达式函数
```lumyr
// 正则表达式匹配
result = regex_match("hello123", "\\d+");  // 匹配数字

// 正则表达式搜索
result = regex_search("hello123world", "\\d+");  // 搜索数字

// 正则表达式替换
result = regex_replace("2026-09-07", "(\\d+)-(\\d+)-(\\d+)", "\\3/\\2/\\1");  // 07/09/2026
result = regex_replace("aaa", "a", "b");  // bbb
```

### 15.4 字符串插值（f-string）

#### 基本用法
```lumyr
name = "World";
print(f"Hello, {name}!");  // "Hello, World!"
```

#### 高级用法
```lumyr
n = 42;

// 表达式内插
print(f"val={n} sq={n * n} even={n % 2 == 0}");

// 方法调用内插
nm = "lumyr";
print(f"[{nm.strip().toupper()}]");  // [LUMYR]

// 函数调用内插
print(f"sq2={squares(3)[1]}");

// 转义花括号（{{ 和 }}）
print(f"{{literal}} n={n}");  // {literal} n=42

// 原样输出（不二次解析）
server = "hello {{name}}";
print(f"{server}");  // hello {{name}}

// 内插表达式内字符串（用 \" 转义）
print(f"v={\"a{b}c\".len()}");  // v=5

// 三元运算符内插
print(f"t={n > 5 ? \"yes\" : \"no\"}");
```

---

## 16. 运算符

### 16.1 算术运算符
| 运算符 | 说明 |
|--------|------|
| `+` | 加法 |
| `-` | 减法 |
| `*` | 乘法 |
| `/` | 除法 |
| `%` | 取模 |
| `^` | 幂运算 |

### 16.2 比较运算符
| 运算符 | 说明 |
|--------|------|
| `==` | 等于 |
| `!=` | 不等于 |
| `<` | 小于 |
| `>` | 大于 |
| `<=` | 小于等于 |
| `>=` | 大于等于 |

### 16.3 逻辑运算符
| 运算符 | 说明 |
|--------|------|
| `&&` | 逻辑与 |
| `||` | 逻辑或 |
| `!` | 逻辑非 |

### 16.4 位运算符
| 运算符 | 说明 |
|--------|------|
| `&` | 按位与 |
| `|` | 按位或 |
| `~` | 按位取反 |
| `<<` | 左移 |
| `>>` | 右移 |

### 16.5 赋值运算符
| 运算符 | 说明 |
|--------|------|
| `=` | 赋值 |
| `+=` | 加并赋值 |
| `-=` | 减并赋值 |
| `*=` | 乘并赋值 |
| `/=` | 除并赋值 |
| `%=` | 取模并赋值 |
| `++` | 自增（前缀/后缀） |
| `--` | 自减（前缀/后缀） |

### 16.6 空安全运算符
| 运算符 | 说明 |
|--------|------|
| `?.` | 安全调用 |
| `??` | 空值合并 |

### 16.7 三元运算符
| 运算符 | 说明 | 示例 |
|--------|------|------|
| `?:` | 条件表达式 | `x > 0 ? "pos" : "neg"` |

```lumyr
r = choose(2) > 0 ? "pos" : "neg";
```

---

## 17. 模块系统

### 17.1 import（带别名）
```lumyr
// 导入模块并起别名
import "math.lm" as m;
print(m.add(1, 2));

// 链式导入
import "chain_b.lm" as b;
print(b.from_b());
```

### 17.2 export
```lumyr
// 导出函数
export func add(a, b) { return a + b; }
export func sub(a, b) { return a - b; }
```

### 17.3 模块特性
- **重复导入去重**：同一模块 import 两次不会冲突
- **循环依赖支持**：cycle_a.lm 和 cycle_b.lm 互相导入可以正常工作
- **链式导入**：A 导入 B，B 导入 C，A 可以通过 B 间接使用 C

---

## 18. 标准库

### 18.1 文件读写语句

#### write 语句（写文件）
```lumyr
// 写文件：write "path" content
write "/tmp/output.txt" "hello world";

// 支持 f-string 内容
n = 42;
write "/tmp/data.txt" f"n={n}";

// 支持 f-string 路径
idx = 1;
write f"/tmp/data_{idx}.txt" "x";
```

#### read 语句（读文件）
```lumyr
// 读文件：read "path" 返回文件内容
content = read "/tmp/output.txt";
print(content);

// 方法链
lines = split((read "/tmp/ft_io.txt").strip(), "\n");
```

### 18.2 时间与睡眠

#### sleep（睡眠）
```lumyr
// 睡眠指定毫秒数
sleep(1000);  // 睡眠 1 秒
sleep(30);    // 睡眠 30 毫秒
```

#### 时间函数
```lumyr
// 当前时间（毫秒级时间戳，返回时间对象）
n = now();
print(n.year);    // 年
print(n.month);   // 月（1-12）
print(n.day);     // 日（1-31）
print(n.hour);    // 时（0-23）
print(n.minute);  // 分（0-59）
print(n.second);  // 秒（0-59）

// Unix 时间戳（秒）
ts = timestamp();

// Unix 时间戳（毫秒）
ts_ms = timestamp_ms();

// 当前日期（YYYY-MM-DD）
d = date();
print(len(d) == 10);  // true

// 当前时间（HH:MM:SS）
t = time();
print(len(t) == 8);   // true

// 当前日期时间（YYYY-MM-DD HH:MM:SS）
dt = datetime();
print(len(dt) == 19); // true

// 格式化时间
formatted = format_time("%Y/%m/%d");
```

### 18.3 HTTP 请求库（requests）
```lumyr
// GET 请求
resp = requests.get("http://example.com/api");
print(resp["body"]);
print(resp["status"]);

// 带参数和请求头的 GET
resp = requests.get("http://example.com/api", {"q": 1}, {"headers": "text/plain"});

// POST 请求
resp = requests.post("http://example.com/api", {"a": 1}, {"body": 42});

// DELETE 请求
resp = requests.delete("http://example.com/api/resource");
```

### 18.3 日志库（log）

#### 全局日志函数
```lumyr
debug("debug message");
info("info message");
warn("warning message");
error("error message");
fatal("fatal message");
```

#### log 对象方法
```lumyr
log.debug("debug message");
log.info("info message");
log.warn("warning message");
log.error("error message");
log.fatal("fatal message");
```

### 18.4 加密库（crypto）
```lumyr
// MD5 哈希
hash = md5("hello world");
print(hash);

// 字符串方法形式
print("hello".md5());
```

### 18.5 JSON 库

#### json_parse / json_stringify
```lumyr
// 解析 JSON
obj = json_parse('{"name": "lumyr", "version": 1}');
print(obj["name"]);

// 序列化 JSON
json_str = json_stringify({"name": "lumyr", "version": 1});
print(json_str);
```

#### json / stringify（简写形式，支持编码参数）
```lumyr
// 解析 JSON（默认 UTF-8）
obj = json('{"name": "张三"}');
print(obj.name);

// 解析 JSON（指定编码）
obj = json(jg, "gbk");

// 序列化 JSON（默认 UTF-8）
json_str = stringify({"name": "张三"});

// 序列化 JSON（指定编码）
json_str = stringify(o, "gbk");
```

### 18.6 查询字符串（qs）

#### qs 函数（Map 转查询字符串）
```lumyr
m = {"name": "张三", "age": 25, "ok": true};

// 默认 UTF-8（中文原样输出）
print(qs(m));  // name=张三&age=25&ok=true

// 指定编码（中文 %XX 编码）
print(qs(m, "gbk"));  // name=%D5%C5%C8%FD&age=25&ok=true
```

#### qs 解析（查询字符串转 Map）
```lumyr
// 默认 UTF-8
r = qs("name=%E5%BC%A0%E4%B8%89&age=25");
print(r.name);  // 张三

// 指定编码
r = qs("name=%D5%C5%C8%FD&age=25", "gbk");
print(r.name);  // 张三
```

#### Map 方法形式
```lumyr
qs = {"n": 1, "m": 2}.qs();  // "n=1&m=2"
```

---

## 19. 内置函数

### 19.1 常用函数
- `print(...)` — 打印
- `len(x)` — 长度
- `type(x)` — 类型
- `keys(m)` — map 的键列表
- `next(g)` — 生成器下一个值
- `string(x)` — 转字符串
- `int(x)` — 转整数
- `double(x)` — 转浮点数

### 19.2 GC 与内存统计
- `gc_count()` — 当前活跃对象数量
- `gc_bytes()` — 当前活跃对象占用的字节数

```lumyr
print("count=" + gc_count() + " bytes=" + gc_bytes());
```

### 19.2 数组与集合函数

#### range（生成范围数组）
```lumyr
arr = range(5);      // [0, 1, 2, 3, 4]
arr = range(20);     // 预分配 20 槽
```

#### map（映射）
```lumyr
// 函数形式
result = map([1, 2, 3], func(v) { return v * 2; });  // [2, 4, 6]

// 方法形式
result = [1, 2, 3].map(func(v) { return v * v; });    // [1, 4, 9]
```

#### reduce（归约）
```lumyr
result = reduce([1, 2, 3, 4], func(a, b) { return a + b; }, 0);  // 10
```

#### filter（过滤）
```lumyr
result = filter([1, 2, 3, 4, 5], func(v) { return v % 2 == 0; });  // [2, 4]
```

### 19.3 生成器函数
- `GenThrow(gen, err)` — 向生成器抛出异常
- `chain(g1, g2)` — 连接生成器
- `zip(g1, g2)` — 合并生成器
- `map(g, fn)` — 映射生成器
- `filter(g, fn)` — 过滤生成器

### 18.3 线程与并发

#### 线程创建
```lumyr
// 创建线程，执行函数并传递参数
t = thread(worker_func, arg);
t2 = thread(worker_func2);  // 无参数

// 等待线程结束
join(t);
```

#### 互斥锁（Mutex）
```lumyr
ml = mutex();          // 创建互斥锁
lock(ml);              // 加锁
// 临界区代码
unlock(ml);            // 解锁
```

#### 读写锁（RwLock）
```lumyr
rw = rwlock();         // 创建读写锁
rdlock(rw);            // 加读锁
// 读临界区
unlock(rw);            // 解锁

wrlock(rw);            // 加写锁
// 写临界区
unlock(rw);            // 解锁

// 尝试加锁（非阻塞）
tryrdlock(rw);         // 尝试加读锁
trywrlock(rw);         // 尝试加写锁
```

#### 自旋锁（Spinlock）
```lumyr
sp = spinlock();       // 创建自旋锁
lock(sp);              // 加锁
unlock(sp);            // 解锁
```

#### 递归锁（Recursive Mutex）
```lumyr
rm = rmutex();         // 创建递归互斥锁
lock(rm);              // 外层加锁
lock(rm);              // 递归可重入（不会死锁）
unlock(rm);            // 释放内层
unlock(rm);            // 释放外层
```

#### 非阻塞尝试加锁（trylock）
```lumyr
// 互斥锁尝试加锁
ok = trylock(ml);      // 成功返回 true，失败返回 false
if (ok) {
    // 临界区
    unlock(ml);
}

// 读写锁尝试加锁
tryrdlock(rw);         // 尝试加读锁
trywrlock(rw);         // 尝试加写锁
```

#### 条件变量（Condition）
```lumyr
cond = cond();         // 创建条件变量
wait(cond, mutex);     // 等待条件
signal(cond);          // 唤醒一个等待线程
broadcast(cond);       // 唤醒所有等待线程

// 带超时等待
cond_wait_timeout(cond, mutex, timeout_ms);
```

#### 线程返回值（thread_join）
```lumyr
func worker(n) {
    return n * 2;
}
t = thread(worker, 21);
result = thread_join(t);   // 等待线程结束并获取返回值
print(result);              // 42
```

#### 线程本地存储（ThreadLocal）
```lumyr
// 设置线程本地变量
threadlocal_set("key", value);

// 获取线程本地变量
val = threadlocal_get("key");
```

### 19.4 数组与 Map 方法

#### 数组方法
```lumyr
arr = [1, 2, 3];

// 添加与删除
arr = arr.add(4);              // 添加元素 → [1,2,3,4]
arr = arr.addAll([5, 6]);      // 批量添加（引用语义：原地修改）
arr = arr.remove(1);            // 删除下标 1 的元素
arr = arr.del(1);               // 同 remove
arr = arr.insert(1, 99);        // 在下标 1 插入 99
arr = arr.set(0, 99);           // 设置下标 0 的元素为 99 → [99,2,3]
arr = arr.clear();              // 清空数组

// 变换与查询
sorted = arr.sort();             // 排序
reversed = arr.reverse();        // 反转
sum_val = arr.sum();             // 求和
avg_val = arr.avg();             // 求平均值
has_2 = arr.contains(2);         // 是否包含

// 获取首尾元素
first_val = arr.first();         // 第一个元素（空数组返回 null）
last_val = arr.last();           // 最后一个元素（空数组返回 null）

// 扁平化
nested = [[1], [2], [3, 4]];
flat = nested.flat(2);           // 扁平化 2 层 → [1,2,3,4]
flat_all = nested.flat(-1);      // 扁平化无限深度（-1 表示全展开）

// 映射与过滤
result = arr.map(func(v) { return v * 2; });   // 映射
result = arr.filter(func(v) { return v > 1; }); // 过滤

// 方法链
x = [].add(1).add(2).add(3).add(4);
print(join([3, 1, 2].sort(), ","));  // "1,2,3"
print([1].addAll([2, 3]).map(dbl).join(","));  // "2,4,6"
```

**注意**：数组方法是引用语义，`add`/`addAll`/`set`/`remove`/`del`/`clear` 会原地修改原数组。

#### Map 方法
```lumyr
m = {"a": 1, "b": 2};

// 方法调用
m.set("c", 3);                  // 设置键值
m.add("d", 4);                  // 添加键值（同 set）
keys = m.keys();                 // 获取键列表
values = m.values();             // 获取值列表
val = m.get("a");                // 获取值（方法形式）
has_a = m.has("a");              // 是否包含键

// 删除与清空
m.del("a");                      // 删除键（引用语义：原地修改）
m.clear();                       // 清空 Map

// 获取首尾键值（按插入序）
first_val = m.first();           // 首键值
last_val = m.last();             // 尾键值

// 批量合并（引用语义：原地修改）
m1 = {"a": 1, "b": 2};
m2 = {"c": 3, "d": 4};
m1.addAll(m2);                   // 合并 m2 到 m1

// 转查询字符串
qs = {"n": 1, "m": 2}.qs();     // "n=1&m=2"

// 方法链
print(m.add("z", 3).len());      // 添加后获取长度
print(m.set("w", 4).len());      // 设置后获取长度
print(m.del("x").len());          // 删除后获取长度
```

#### Map 点属性访问

Map 支持点语法访问和赋值属性（等价于下标访问）。

```lumyr
m = {"a": 1, "b": "x", "c": [1, 2]};

// 点属性读
print(m.a);                      // 1（等价 m["a"]）
print(m.b);                      // "x"
print(m.c[1]);                   // 2（属性取到数组再下标）

// 点属性赋值
m.d = 42;                        // 新增键
m.a = 100;                       // 覆盖已有键
m.e = [7, 8];

// 嵌套组合
cfg = {"data": [1, 2, 3], "opts": {"flag": true}};
cfg.data = cfg.data.add(4);     // 属性读+数组方法+属性赋值
print(cfg.data.remove(0)[0]);   // 2
```

---

## 20. 重要语法规则总结

### 20.1 类型声明 vs 类型转换
| 语法 | 用途 | 示例 |
|------|------|------|
| `<>` | 类型声明/标注 | `a = <int>8;` `func f(<int> x)` |
| `()` | 类型转换（强转） | `a = (int)3.14;` |

### 20.2 参数类型
| 语法 | 说明 |
|------|------|
| `p` | 普通参数（值传递） |
| `<int> p` | 带类型标注的普通参数 |
| `ref p` | 引用传递参数 |
| `ref <int> p` | 带类型标注的引用传递参数 |
| `p = 10` | 带默认值的参数 |
| `...args` | 可变参数 |

### 20.3 函数类型
| 语法 | 说明 |
|------|------|
| `func name() {}` | 普通函数 |
| `gen func name() {}` | 生成器函数 |
| `const func name() {}` | 编译期常量函数 |
| `func<T> name() {}` | 泛型函数 |
| `extern func name();` | FFI 外部函数声明 |

### 20.4 循环类型
| 语法 | 说明 |
|------|------|
| `while (cond) {}` | while 循环 |
| `do {} while (cond);` | do-while 循环 |
| `for (init; cond; incr) {}` | C 风格 for 循环 |
| `for x in arr {}` | for-each 数组循环 |
| `for k, v in map {}` | for-each map 循环 |
| `for x iter obj {}` | 迭代器协议循环 |

---

## 21. 常见错误

### 21.1 忘记分号
```lumyr
// 错误（会报"语法错误(第2行)"）
print("hello")

// 正确
print("hello");
```

### 20.2 类型声明用错语法（冒号 vs 尖括号）
```lumyr
// 错误（冒号不是类型声明，会报语法错误）
func f(p: Point) {}
func modify(ref p: Point) {}

// 正确（尖括号是类型声明）
func f(<Point> p) {}
func modify(ref <Point> p) {}
```

### 20.3 类型转换用错语法
```lumyr
// 错误（尖括号不是类型转换，是类型声明）
x = <int>3.14;

// 正确（括号是类型转换）
x = (int)3.14;
```

### 20.4 ref 参数缺少类型标注时的语法
```lumyr
// 正确（ref 参数可以不带类型标注）
func modify(ref p) {}

// 正确（ref 参数带类型标注）
func modify(ref <Point> p) {}
```

### 20.5 复合表达式类型标注的优先级
```lumyr
// <int8>(100 + 900)：先计算再截断，结果 -24
print(<int8>(100 + 900));

// <int8>100 + 900：只截断 100，再加 900，结果 1000
print(<int8>100 + 900);
```

### 20.6 lambda 闭包限制
```lumyr
// 错误：lambda 不能访问外层函数局部变量（编译期禁止）
func outer() {
    x = 10;
    f = func() { return x; };  // 错误：x 是外层局部变量
}

// 正确：lambda 只能访问全局变量和自身参数
g_x = 10;  // 全局变量
f = func() { return g_x; };  // 正确
```

### 20.7 函数内赋值总是局部（词法遮蔽）
```lumyr
counter = 0;  // 全局变量
bump = func() {
    counter = counter + 1;  // 这里的 counter 是局部变量，不是全局
    return counter;
};
print(bump());   // 1（读全局 counter=0，+1=1，但写入为局部）
print(bump());   // 1（每次调用都读全局 counter=0）
print(counter);  // 0（全局 counter 从未被修改）
```

---

*本文档基于 Lumyr 编译器源码和 90+ 测试文件整理，如有疑问请参考 `src/parse/yacc.y` 和 `tests/` 目录。*
