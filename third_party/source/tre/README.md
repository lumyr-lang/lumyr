# TRE 源码

轻量级正则表达式匹配库，支持近似匹配（模糊匹配）。

## 版本
- 当前使用版本：0.8.0
- Windows 预编译版本：libtre.a（MinGW 静态库）

## 获取源码

### 方法1：从 GitHub 下载
```bash
wget https://github.com/laurikari/tre/releases/download/0.8.0/tre-0.8.0.tar.gz
tar -xzf tre-0.8.0.tar.gz
# 将解压后的内容复制到本目录
```

### 方法2：使用 Git 克隆
```bash
git clone https://github.com/laurikari/tre.git .
git checkout 0.8.0
```

## 编译

### Windows (MinGW/MSYS2)
```bash
./autogen.sh  # 如果从 git 克隆
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install
```

### macOS
```bash
brew install tre
# 或从源码编译
./autogen.sh
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install libtre-dev

# 或从源码编译
./autogen.sh
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

## 依赖
- 无强制依赖
- libiconv（可选，用于多字节字符支持）
- gettext（可选，用于国际化支持）

## 特性
- POSIX 兼容的正则表达式语法
- 近似匹配（模糊匹配），支持插入、删除、替换操作
- 宽字符支持
- 高性能，内存占用小

## 许可证
- BSD 2-Clause 许可证
- 详见：https://github.com/laurikari/tre/blob/master/LICENSE
