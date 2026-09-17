# flex 源码

词法分析器生成工具，用于生成词法分析器（lexer）。

## 版本
- 当前使用版本：2.6.4
- Windows 版本：WinFlexBison 2.5.25（包含 flex 2.6.4）

## 获取源码

### 方法1：从 GitHub 下载
```bash
wget https://github.com/westes/flex/releases/download/v2.6.4/flex-2.6.4.tar.gz
tar -xzf flex-2.6.4.tar.gz
# 将解压后的内容复制到本目录
```

### 方法2：使用 Git 克隆
```bash
git clone https://github.com/westes/flex.git .
git checkout v2.6.4
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
brew install flex
# 或从源码编译
./autogen.sh
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install flex

# 或从源码编译
./autogen.sh
./configure --prefix=/usr/local --disable-shared --enable-static
make
sudo make install
```

## 依赖
- bison（编译时需要）
- m4（运行时需要）
- libiconv（可选，用于多字节字符支持）

## 许可证
- BSD 许可证（flex 2.6.4 及以上版本）
- 详见：https://github.com/westes/flex/blob/master/COPYING
