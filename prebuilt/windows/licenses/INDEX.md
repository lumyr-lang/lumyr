# 第三方组件许可证 — Windows (x64, MinGW-w64)

本目录是 `bin\lumyr.exe` 所链接第三方组件的**许可证原文汇总**，随 git 提交。
组件由 `scripts/build_deps_mingw.sh` 从 `scripts/vendor` 源码构建，版本锁定一致。

| 组件 | 版本 | 链接方式 | 许可证 | 原文 | 源码 |
|------|------|----------|--------|------|------|
| libcurl | 8.22.0 | 静态 | MIT/X 派生 | [libcurl/COPYING](libcurl/COPYING) | https://curl.se/download/curl-8.22.0.tar.gz |
| TRE | 0.9.0 | 静态 | BSD 2-Clause | [tre/LICENSE](tre/LICENSE) | https://github.com/laurikari/tre |
| libiconv | 1.17 | **动态** libiconv-2.dll | LGPL-2.1 | [libiconv/COPYING.LGPL-2.1](libiconv/COPYING.LGPL-2.1) | https://ftp.gnu.org/gnu/libiconv/libiconv-1.17.tar.gz |

## 合规说明

- **libiconv 为 LGPL，采用动态链接**：libiconv-2.dll 与 lumyr.exe 同目录，
  用户可用自行编译的 libiconv-2.dll 直接替换，满足 LGPL 的可重新链接义务。
  LGPL-2.1 完整条款见 libiconv/COPYING.LGPL-2.1。
- libcurl 的 TLS 后端为 **Windows 原生 Schannel**，不使用、不分发 OpenSSL。
- GMP 6.3.0（LGPLv3，动态 libgmp-10.dll）许可证见
  [deps/gmp/licenses](../../../deps/gmp/licenses/)，打包发行时同样需包含。
- 静态链接的 curl/TRE 为宽松许可证，义务仅为再分发时保留本声明。
