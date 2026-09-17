# m4 源码

GNU M4 宏处理器，是 bison 的运行时依赖。

## 版本
- 当前使用版本：1.4.19
- Windows 版本：m4.zip（可能包含预编译版本）

## 获取源码

### 方法1：从 GNU 官网下载
```bash
wget https://ftp.gnu.org/gnu/m4/m4-1.4.19.tar.gz
tar -xzf m4-1.4.19.tar.gz
# 将解压后的内容复制到本目录
```

### 方法2：使用 Git 克隆
```bash
git clone https://git.savannah.gnu.org/git/m4.git .
git checkout v1.4.19
```

## 编译

### Windows (MinGW/MSYS2)
```bash
./configure --prefix=/path/to/install --disable-shared --enable-static
make
make install
```

### macOS
```bash
brew install m4
# 或从源码编译
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install m4

# 或从源码编译
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

## 依赖
- 无强制依赖
- libiconv（可选，用于多字节字符支持）
- gettext（可选，用于国际化支持）

## 说明
m4 是 bison 的运行时依赖，bison 在生成语法分析器时需要调用 m4 来处理宏。
如果系统中已经安装了 bison，通常 m4 也会作为依赖自动安装。

## 许可证
- GNU GPL v3 许可证
- 详见：https://www.gnu.org/software/m4/
