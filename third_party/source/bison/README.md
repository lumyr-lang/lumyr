# bison 源码

语法分析器生成工具，用于生成语法分析器（parser）。

## 版本
- 当前使用版本：3.8.2
- Windows 版本：WinFlexBison 2.5.25（包含 bison 3.8.2）

## 获取源码

### 方法1：从 GNU 官网下载
```bash
wget https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.gz
tar -xzf bison-3.8.2.tar.gz
# 将解压后的内容复制到本目录
```

### 方法2：使用 Git 克隆
```bash
git clone https://git.savannah.gnu.org/git/bison.git .
git checkout v3.8.2
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
brew install bison
# 或从源码编译
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install bison

# 或从源码编译
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

## 依赖
- m4（运行时必需）
- flex（编译时需要，可选）
- libiconv（可选，用于多字节字符支持）

## 许可证
- GNU GPL v3 许可证
- 详见：https://www.gnu.org/software/bison/manual/html_node/Conditions.html
