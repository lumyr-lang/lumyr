# libcurl 源码

HTTP 客户端库，用于网络请求。

## 版本
- 当前使用版本：8.22.0
- Windows 预编译版本：curl-8.22.0_1-win64-mingw

## 获取源码

### 方法1：从官方网站下载
```bash
wget https://curl.se/download/curl-8.22.0.tar.gz
tar -xzf curl-8.22.0.tar.gz
# 将解压后的内容复制到本目录
```

### 方法2：使用 Git 克隆
```bash
git clone https://github.com/curl/curl.git .
git checkout curl-8_22_0
```

## 编译

### Windows (MinGW)
```bash
./configure --prefix=/path/to/install --with-openssl --disable-shared --enable-static
make
make install
```

### macOS
```bash
brew install curl
# 或从源码编译
./configure --prefix=/usr/local --with-openssl --disable-shared --enable-static
make
sudo make install
```

### Linux
```bash
# Debian/Ubuntu
sudo apt install libcurl4-openssl-dev

# 或从源码编译
./configure --prefix=/usr/local --with-openssl --disable-shared --enable-static
make
sudo make install
```

## 依赖
- OpenSSL（或其他 TLS 库）
- zlib
- libpsl（可选）
- libssh2（可选）

## 许可证
- MIT/X  derivate 许可证（类似 BSD）
- 详见：https://curl.se/docs/copyright.html
