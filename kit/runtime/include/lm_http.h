#ifndef LM_HTTP_H
#define LM_HTTP_H

#include "lumyr_value_type.h"
#include "lumyr_value.h"

// 发 HTTP 请求（libcurl）
//   method : "GET"/"POST"/"PUT"/"DELETE"/"HEAD"/"PATCH" 等
//   url    : VAL_STRING，必填
//   config : VAL_MAP，可选键：params/headers/body/form/files/output/
//            responseType/timeout/allowRedirects（详见 lm_http.c 文件头注释）
// 返回    : VAL_MAP {status:int, headers:map, body:自动类型, bytes:bytes, json:解析值}
// 参数/连接/协议错误 → VAL_ERROR（调用方 throw，可 try/catch）
Value lumyr_http_request(const char* method, Value url, Value config);

#endif //LM_HTTP_H
