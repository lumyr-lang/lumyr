#ifndef VM_SERIALIZE_H
#define VM_SERIALIZE_H

#include "lumyr_value.h"

/* 对象二进制序列化（紧凑格式，大端字节序）。
 * 所有失败均为硬错误（fprintf + exit(1)，无运行时兜底）。 */

/* 单值编码 → VAL_BYTES（不含流头）。
 * class/struct/type 实例必须实现 Serializable 接口，否则硬错误。 */
Value lm_serialize_value(Value v);

/* 在 b（VAL_BYTES）的 offset 处解码一个值：
 * outVal 写入还原值，返回新的读取偏移。流损坏/类型未注册等硬错误。 */
int lm_deserialize_value_at(Value b, int offset, Value* outVal);

/* 流构建：写流头（magic + version + flags）并顺序拼接 chunks
 * （chunks 为 VAL_ARRAY，元素必须是 VAL_BYTES）→ VAL_BYTES */
Value lm_build_stream(Value chunks);

/* 校验流头：成功返回载荷起始偏移；magic/version 不匹配硬错误 */
int lm_check_stream_header(Value b);

#endif /* VM_SERIALIZE_H */
