/*
 * vm_serialize.c - 对象二进制序列化编解码（ObjectStream 底层实现）
 *
 * 格式（大端 / network byte order）：
 *   流头：'L' 'M' 'O 'S' + version(1) + flags(1)            共 6 字节
 *   值：tag(1) + payload，tag 见 SerTag
 *
 * 语义宽化（第一版）：整数族统一按 int64 写/读（VAL_INT），
 *   浮点族统一按 double；写回实例字段时由 lumyr_field_set 按字段类型精确
 *   转换，故实例字段不丢精度；顶层/容器元素会宽化。
 */
#include "vm_serialize.h"
#include "gc_runtime.h"
#include "lm_container.h"
#include "lm_type.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* ==================== 常量 ==================== */

#define SER_MAGIC0 'L'
#define SER_MAGIC1 'M'
#define SER_MAGIC2 'O'
#define SER_MAGIC3 'S'
#define SER_VERSION 1

typedef enum {
    TAG_NULL        = 0x01,
    TAG_BOOL        = 0x02,
    TAG_INT         = 0x03,  /* int64 BE */
    TAG_DOUBLE      = 0x04,  /* IEEE754 BE */
    TAG_CHAR        = 0x05,  /* 1 字节 */
    TAG_BYTE        = 0x06,  /* 1 字节 */
    TAG_STRING      = 0x07,  /* be32 len + UTF-8 */
    TAG_ARRAY       = 0x08,  /* be32 n + values */
    TAG_MAP         = 0x09,  /* be32 n + (key value) pairs */
    TAG_TYPED_ARRAY = 0x0A,  /* be32 elemType + be32 n + values */
    TAG_BYTES       = 0x0B,  /* be32 len + raw */
    TAG_OBJECT      = 0x0C,  /* be32 nameLen + name + be32 fieldN + (name value) */
    TAG_TUPLE       = 0x0D,  /* be32 n + values */
    TAG_SET         = 0x0E   /* be32 n + values */
} SerTag;

/* ==================== 通用错误 ==================== */

static void serFatal(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "序列化错误 / Serialization error: ");
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

/* ==================== 类型族判断 / 取值 ==================== */

/* 任意数值 Value → int64（与 vm_builtin bi_num_i64 同语义） */
static int64_t numToI64(Value v) {
    switch(v.type) {
    case VAL_INT64: return v.v.i64;
    case VAL_INT:   return (int64_t)v.v.i;
    case VAL_INT8:  return (int64_t)v.v.i8;
    case VAL_INT16: return (int64_t)v.v.i16;
    case VAL_INT32: return (int64_t)v.v.i32;
    case VAL_LONG:  return (int64_t)v.v.l;
    case VAL_LONG_LONG: return (int64_t)v.v.ll;
    case VAL_SHORT: return (int64_t)v.v.sh;
    case VAL_UINT8: return (int64_t)v.v.u8;
    case VAL_UCHAR: return (int64_t)v.v.uc;
    case VAL_BYTE:  return (int64_t)v.v.by;
    case VAL_UINT16: return (int64_t)v.v.u16;
    case VAL_USHORT: return (int64_t)v.v.us;
    case VAL_UINT32: return (int64_t)v.v.u32;
    case VAL_UINT:  return (int64_t)v.v.ui;
    case VAL_UINT64: return (int64_t)v.v.u64;
    case VAL_ULONG: return (int64_t)v.v.ul;
    case VAL_SIZE_T: return (int64_t)v.v.st;
    case VAL_SSIZE_T: return (int64_t)v.v.sst;
    case VAL_BOOL:  return (int64_t)v.v.b;
    case VAL_CHAR:  return (int64_t)v.v.c;
    case VAL_DOUBLE: return (int64_t)v.v.d;
    case VAL_FLOAT: return (int64_t)v.v.f;
    case VAL_LONG_DOUBLE: return (int64_t)v.v.ld;
    default:        return 0;
    }
}

static double numToDouble(Value v) {
    switch(v.type) {
    case VAL_DOUBLE: return v.v.d;
    case VAL_FLOAT:  return (double)v.v.f;
    case VAL_LONG_DOUBLE: return (double)v.v.ld;
    case VAL_STRING: return 0.0;
    default:         return (double)numToI64(v);
    }
}

/* 取字符串 C 指针：必须接收 Value 指针——SSO 内联串的字节存储在 Value
 * 自身内部，按值传参会产生副本、返回地址随副本释放而悬空。
 * 调用方须保证在同一作用域内立即消费（编码处即刻 memcpy，安全）。 */
static const char* strPtr(const Value* v) {
    if(v->type != VAL_STRING) return NULL;
    return v->str_inline ? v->v.sso.data : v->v.s;
}

/* ==================== 编码缓冲 ==================== */

typedef struct {
    uint8_t* d;
    int len;
    int cap;
} SerBuf;

static void sbReserve(SerBuf* b, int need) {
    if(need <= b->cap) return;
    int nc = b->cap > 0 ? b->cap * 2 : 64;
    while(nc < need) nc *= 2;
    uint8_t* p = (uint8_t*)realloc(b->d, (size_t)nc);
    if(!p) serFatal("内存不足 / out of memory");
    b->d = p;
    b->cap = nc;
}

static void sbU8(SerBuf* b, uint8_t x) {
    sbReserve(b, b->len + 1);
    b->d[b->len++] = x;
}

static void sbBE32(SerBuf* b, uint32_t x) {
    sbReserve(b, b->len + 4);
    b->d[b->len++] = (uint8_t)(x >> 24);
    b->d[b->len++] = (uint8_t)(x >> 16);
    b->d[b->len++] = (uint8_t)(x >> 8);
    b->d[b->len++] = (uint8_t)x;
}

static void sbBE64(SerBuf* b, uint64_t x) {
    sbReserve(b, b->len + 8);
    for(int i = 7; i >= 0; i--)
        b->d[b->len++] = (uint8_t)(x >> (i * 8));
}

static void sbRaw(SerBuf* b, const void* p, int n) {
    if(n <= 0) return;
    sbReserve(b, b->len + n);
    memcpy(b->d + b->len, p, (size_t)n);
    b->len += n;
}

/* be32 长度 + 原始字节（字符串/字节串通用） */
static void sbLenData(SerBuf* b, const void* p, int n) {
    sbBE32(b, (uint32_t)n);
    sbRaw(b, p, n);
}

/* ==================== 编码 ==================== */

/* TypedArray 元素读取（与 vm_builtin bi_read_*_et 同语义：
 * 有符号/无符号按精确 C 指针类型读取，避免符号扩展错误） */
static int64_t taReadI64(TypedArray* ta, int i) {
    ValueType et = ta->elem_type;
    const void* items = ta->items;
    switch(et) {
    case VAL_INT:       return ((const int*)items)[i];
    case VAL_INT8:      return ((const int8_t*)items)[i];
    case VAL_INT16:     return ((const int16_t*)items)[i];
    case VAL_INT32:     return ((const int32_t*)items)[i];
    case VAL_INT64:     return ((const int64_t*)items)[i];
    case VAL_LONG_LONG: return ((const long long*)items)[i];
    case VAL_LONG:      return ((const long*)items)[i];
    case VAL_UINT8:     return ((const uint8_t*)items)[i];
    case VAL_UINT16:    return ((const uint16_t*)items)[i];
    case VAL_UINT32:    return ((const uint32_t*)items)[i];
    case VAL_UINT:      return ((const unsigned int*)items)[i];
    case VAL_UINT64:    return ((const uint64_t*)items)[i];
    case VAL_ULONG:     return ((const unsigned long*)items)[i];
    case VAL_UCHAR:     return ((const unsigned char*)items)[i];
    case VAL_SHORT:     return ((const short*)items)[i];
    case VAL_USHORT:    return ((const unsigned short*)items)[i];
    case VAL_SIZE_T:    return (int64_t)((const size_t*)items)[i];
    case VAL_SSIZE_T:   return (int64_t)((const ssize_t*)items)[i];
    case VAL_BOOL:      return ((const _Bool*)items)[i];
    case VAL_CHAR:      return ((const char*)items)[i];
    case VAL_BYTE:      return ((const uint8_t*)items)[i];
    case VAL_DOUBLE:    return (int64_t)((const double*)items)[i];
    case VAL_FLOAT:     return (int64_t)((const float*)items)[i];
    case VAL_LONG_DOUBLE: return (int64_t)((const long double*)items)[i];
    default:            return 0;
    }
}

static Value taReadDouble(TypedArray* ta, int i) {
    Value r = val_none();
    ValueType et = ta->elem_type;
    r.type = et;
    if(et == VAL_DOUBLE)          r.v.d = ((const double*)ta->items)[i];
    else if(et == VAL_FLOAT)      r.v.f = ((const float*)ta->items)[i];
    else                          r.v.ld = ((const long double*)ta->items)[i];
    return r;
}

static char* taReadPtr(TypedArray* ta, int i) {
    return ta->items ? ((char**)ta->items)[i] : NULL;
}

static void encValue(SerBuf* b, Value v);

static void encInstance(SerBuf* b, Value v) {
    RuntimeTypeInfo* info = lumyr_instance_get_info(v);
    if(!info || !info->name)
        serFatal("无法获取实例类型信息 / cannot get instance type info");
    if(!lumyr_type_implements(info, "Serializable"))
        serFatal("类型 '%s' 未实现 Serializable 接口，不可序列化 / "
                 "type '%s' does not implement Serializable",
                 info->name, info->name);
    const char* nm = info->name;
    size_t nlen = strlen(nm);
    if(nlen > 0x7FFFFFFFu) serFatal("类型名过长 / type name too long");
    sbU8(b, TAG_OBJECT);
    sbLenData(b, nm, (int)nlen);
    sbBE32(b, (uint32_t)info->nfields);
    for(int i = 0; i < info->nfields; i++) {
        const char* fnm = info->fields[i].name;
        Value fv = lumyr_field_get(v, fnm);
        sbLenData(b, fnm, (int)strlen(fnm));
        encValue(b, fv);
    }
}

static void encValue(SerBuf* b, Value v) {
    switch(v.type) {
    case VAL_NONE:
        sbU8(b, TAG_NULL);
        return;
    case VAL_BOOL:
        sbU8(b, TAG_BOOL);
        sbU8(b, v.v.b ? 1u : 0u);
        return;
    case VAL_CHAR:
        sbU8(b, TAG_CHAR);
        sbU8(b, (uint8_t)v.v.c);
        return;
    case VAL_BYTE:
        sbU8(b, TAG_BYTE);
        sbU8(b, v.v.by);
        return;
    /* 整数族：统一宽化为 int64（第一版语义）。
     * 写回实例字段时由 lumyr_field_set 按字段类型精确转换，不丢精度；
     * 顶层值/容器元素读回为 int64。 */
    case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
    case VAL_LONG: case VAL_LONG_LONG: case VAL_SHORT:
    case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT:
    case VAL_UINT64: case VAL_ULONG: case VAL_UCHAR: case VAL_USHORT:
    case VAL_SIZE_T: case VAL_SSIZE_T: {
        sbU8(b, TAG_INT);
        sbBE64(b, (uint64_t)numToI64(v));
        return;
    }
    case VAL_DOUBLE: case VAL_FLOAT: case VAL_LONG_DOUBLE: {
        sbU8(b, TAG_DOUBLE);
        double d = numToDouble(v);
        uint64_t bits;
        memcpy(&bits, &d, 8);
        sbBE64(b, bits);
        return;
    }
    case VAL_STRING: {
        const char* s = strPtr(&v);
        int n = s ? (int)strlen(s) : 0;
        sbU8(b, TAG_STRING);
        sbLenData(b, s, n);
        return;
    }
    case VAL_ARRAY: {
        ValueArray* a = v.v.array;
        int n = a ? a->len : 0;
        sbU8(b, TAG_ARRAY);
        sbBE32(b, (uint32_t)n);
        for(int i = 0; i < n; i++) encValue(b, a->items[i]);
        return;
    }
    case VAL_MAP: {
        Value ks = lumyr_map_keys(v);
        ValueArray* ka = ks.v.array;
        int n = ka ? ka->len : 0;
        sbU8(b, TAG_MAP);
        sbBE32(b, (uint32_t)n);
        for(int i = 0; i < n; i++) {
            Value key = ka->items[i];
            encValue(b, key);
            encValue(b, lumyr_map_get(v, key));
        }
        return;
    }
    case VAL_TYPED_ARRAY: {
        TypedArray* ta = v.v.typed_array;
        int n = ta ? ta->len : 0;
        ValueType et = ta ? ta->elem_type : VAL_NONE;
        sbU8(b, TAG_TYPED_ARRAY);
        sbBE32(b, (uint32_t)et);
        sbBE32(b, (uint32_t)n);
        for(int i = 0; i < n; i++) {
            /* 装箱为 Value 后递归；元素类型由读侧 elem_type 恢复 */
            Value ev = val_none();
            int cls = lumyr_etype_stackcls(et);
            if(cls == 1) {
                ev.type = et;
                ev.v.ll = taReadI64(ta, i);
            } else if(cls == 2) {
                ev = taReadDouble(ta, i);
            } else if(cls == 3) {
                ev.type = et;
                ev.str_inline = 0;
                ev.v.s = taReadPtr(ta, i);
            }
            encValue(b, ev);
        }
        return;
    }
    case VAL_BYTES: {
        BytesObj* bo = (BytesObj*)v.v.bytes_obj;
        int n = bo ? bo->len : 0;
        sbU8(b, TAG_BYTES);
        sbLenData(b, bo ? bo->data : NULL, n);
        return;
    }
    case VAL_STRUCT_PTR: case VAL_CLASS_PTR:
        encInstance(b, v);
        return;
    case VAL_TUPLE: {
        int n = lumyr_tuple_len(v);
        sbU8(b, TAG_TUPLE);
        sbBE32(b, (uint32_t)n);
        for(int i = 0; i < n; i++) encValue(b, lumyr_tuple_get(v, i));
        return;
    }
    case VAL_SET: {
        Value arr = lumyr_set_to_array(v);
        ValueArray* a = arr.v.array;
        int n = a ? a->len : 0;
        sbU8(b, TAG_SET);
        sbBE32(b, (uint32_t)n);
        for(int i = 0; i < n; i++) encValue(b, a->items[i]);
        return;
    }
    default:
        serFatal("类型 '%s' 不支持序列化 / type '%s' is not serializable",
                 val_typename(v.type), val_typename(v.type));
    }
}

Value lm_serialize_value(Value v) {
    SerBuf b = {NULL, 0, 0};
    encValue(&b, v);
    Value r = lumyr_bytes_from_buf(b.d, b.len);
    free(b.d);
    return r;
}

/* ==================== 解码 ==================== */

typedef struct {
    const uint8_t* d;
    int len;
    int pos;
} DecCursor;

static void dcNeed(DecCursor* c, int n) {
    if(n < 0 || c->pos > c->len - n)
        serFatal("数据流意外结束或已损坏 / unexpected end of stream or corrupted data");
}

static uint8_t dcU8(DecCursor* c) {
    dcNeed(c, 1);
    return c->d[c->pos++];
}

static uint32_t dcBE32(DecCursor* c) {
    dcNeed(c, 4);
    uint32_t x = 0;
    for(int i = 0; i < 4; i++) x = (x << 8) | c->d[c->pos++];
    return x;
}

static uint64_t dcBE64(DecCursor* c) {
    dcNeed(c, 8);
    uint64_t x = 0;
    for(int i = 0; i < 8; i++) x = (x << 8) | c->d[c->pos++];
    return x;
}

/* 读取长度前缀的原始段：返回段长，并经 outData 输出指针（不拷贝） */
static int dcLenData(DecCursor* c, const uint8_t** outData) {
    uint32_t n = dcBE32(c);
    dcNeed(c, (int)n);
    if(outData) *outData = c->d + c->pos;
    c->pos += (int)n;
    return (int)n;
}

/* TypedArray 元素写槽（自包含，参照 vm_builtin bi_ta_write_value） */
static void taSetElem(TypedArray* ta, int i, Value v) {
    ValueType et = ta->elem_type;
    size_t isz = lumyr_etype_itemsz(et);
    if(isz == 0) serFatal("未知的类型化数组元素类型 %d / unknown typed-array element type", (int)et);
    if(et == VAL_DOUBLE || et == VAL_FLOAT || et == VAL_LONG_DOUBLE) {
        double d = numToDouble(v);
        if(et == VAL_DOUBLE)      ((double*)ta->items)[i] = d;
        else if(et == VAL_FLOAT)  ((float*)ta->items)[i] = (float)d;
        else                      ((long double*)ta->items)[i] = (long double)d;
        return;
    }
    if(et == VAL_STRING) {
        const char* s = strPtr(&v);
        int n = s ? (int)strlen(s) : 0;
        char* gs = (char*)gc_alloc((size_t)n + 1, VAL_STRING);
        if(n) memcpy(gs, s, (size_t)n);
        gs[n] = '\0';
        ((char**)ta->items)[i] = gs;
        return;
    }
    if(lumyr_etype_stackcls(et) == 3) {
        void* raw = NULL;
        switch(v.type) {
        case VAL_PTR:         raw = v.v.struct_ptr; break;
        case VAL_STRUCT_PTR:  raw = v.v.struct_ptr; break;
        case VAL_CLASS_PTR:   raw = v.v.struct_ptr; break;
        case VAL_BIGINT:      raw = v.v.bigint; break;
        case VAL_DECIMAL:     raw = v.v.decimal; break;
        case VAL_BITDECIMAL:  raw = v.v.bitdecimal; break;
        case VAL_MAP:         raw = v.v.map; break;
        case VAL_ARRAY:       raw = v.v.array; break;
        case VAL_TYPED_ARRAY: raw = v.v.typed_array; break;
        default: break;
        }
        ((void**)ta->items)[i] = raw;
        return;
    }
    /* 整数族（含 bool/char/byte 及扩展整型）：按元素宽度写补码 */
    int64_t x = numToI64(v);
    memset((char*)ta->items + (size_t)i * isz, 0, isz);
    memcpy((char*)ta->items + (size_t)i * isz, &x, isz);
}

static Value decValue(DecCursor* c);

static Value decTypedArray(DecCursor* c) {
    ValueType et = (ValueType)dcBE32(c);
    int n = (int)dcBE32(c);
    if(n < 0) serFatal("数组长度非法 / invalid array length");
    size_t isz = lumyr_etype_itemsz(et);
    if(isz == 0) serFatal("未知的类型化数组元素类型 %d / unknown typed-array element type", (int)et);
    TypedArray* ta = (TypedArray*)gc_alloc(sizeof(TypedArray), VAL_TYPED_ARRAY);
    ta->len = n;
    ta->cap = n > 0 ? n : 1;
    ta->elem_type = et;
    ta->stack_alloc = 0;
    ta->items = gc_alloc_old(isz * (size_t)ta->cap, VAL_TYPED_ARRAY);
    Value v = val_none();
    v.type = VAL_TYPED_ARRAY;
    v.v.typed_array = ta;
    for(int i = 0; i < n; i++) {
        Value ev = decValue(c);
        taSetElem(ta, i, ev);
    }
    return v;
}

static Value decInstance(DecCursor* c) {
    const uint8_t* nd = NULL;
    int nlen = dcLenData(c, &nd);
    char* name = (char*)malloc((size_t)nlen + 1);
    if(!name) serFatal("内存不足 / out of memory");
    if(nlen) memcpy(name, nd, (size_t)nlen);
    name[nlen] = '\0';
    RuntimeTypeInfo* info = lumyr_type_lookup(name);
    if(!info)
        serFatal("找不到类型 '%s'，无法反序列化 / type '%s' not found, cannot deserialize",
                 name, name);
    Value obj = lumyr_instance_new(info);
    if(obj.type == VAL_NONE)
        serFatal("类型 '%s' 不可实例化（可能为抽象类） / type '%s' cannot be instantiated",
                 name, name);
    int fieldN = (int)dcBE32(c);
    for(int i = 0; i < fieldN; i++) {
        const uint8_t* fd = NULL;
        int flen = dcLenData(c, &fd);
        char* fnm = (char*)malloc((size_t)flen + 1);
        if(!fnm) serFatal("内存不足 / out of memory");
        if(flen) memcpy(fnm, fd, (size_t)flen);
        fnm[flen] = '\0';
        Value fv = decValue(c);
        if(!lumyr_type_find_field(info, fnm))
            serFatal("类型 '%s' 没有字段 '%s' / type '%s' has no field '%s'",
                     name, fnm, name, fnm);
        /* 受信写入：反序列化须能恢复 private 字段（绕过访问控制） */
        lumyr_field_set_trusted(obj, fnm, fv, 0);
        free(fnm);
    }
    free(name);
    return obj;
}

static Value decValue(DecCursor* c) {
    uint8_t tag = dcU8(c);
    switch(tag) {
    case TAG_NULL:
        return val_none();
    case TAG_BOOL:
        return val_bool(dcU8(c) != 0);
    case TAG_INT:
        return val_int((long long)dcBE64(c));
    case TAG_DOUBLE: {
        uint64_t bits = dcBE64(c);
        double d;
        memcpy(&d, &bits, 8);
        return val_double(d);
    }
    case TAG_CHAR:
        return val_char((char)dcU8(c));
    case TAG_BYTE:
        return lumyr_make_byte(dcU8(c));
    case TAG_STRING: {
        const uint8_t* pd = NULL;
        int n = dcLenData(c, &pd);
        char* buf = (char*)malloc((size_t)n + 1);
        if(!buf) serFatal("内存不足 / out of memory");
        if(n) memcpy(buf, pd, (size_t)n);
        buf[n] = '\0';
        Value v = val_string(buf);
        free(buf);
        return v;
    }
    case TAG_ARRAY: {
        int n = (int)dcBE32(c);
        if(n < 0) serFatal("数组长度非法 / invalid array length");
        Value arr = val_array(n);
        for(int i = 0; i < n; i++) arr.v.array->items[i] = decValue(c);
        return arr;
    }
    case TAG_MAP: {
        int n = (int)dcBE32(c);
        if(n < 0) serFatal("字典长度非法 / invalid map length");
        Value m = val_map();
        for(int i = 0; i < n; i++) {
            Value key = decValue(c);
            Value val = decValue(c);
            lumyr_map_set(&m, key, val);
        }
        return m;
    }
    case TAG_TYPED_ARRAY:
        return decTypedArray(c);
    case TAG_BYTES: {
        const uint8_t* pd = NULL;
        int n = dcLenData(c, &pd);
        return lumyr_bytes_from_buf(pd, n);
    }
    case TAG_OBJECT:
        return decInstance(c);
    case TAG_TUPLE: {
        int n = (int)dcBE32(c);
        if(n < 0) serFatal("元组长度非法 / invalid tuple length");
        Value* vals = (Value*)calloc((size_t)(n > 0 ? n : 1), sizeof(Value));
        if(!vals) serFatal("内存不足 / out of memory");
        for(int i = 0; i < n; i++) vals[i] = decValue(c);
        Value t = lumyr_tuple_make(n, vals);
        free(vals);
        return t;
    }
    case TAG_SET: {
        int n = (int)dcBE32(c);
        if(n < 0) serFatal("集合长度非法 / invalid set length");
        Value* vals = (Value*)calloc((size_t)(n > 0 ? n : 1), sizeof(Value));
        if(!vals) serFatal("内存不足 / out of memory");
        for(int i = 0; i < n; i++) vals[i] = decValue(c);
        Value s = lumyr_set_make(n, vals);
        free(vals);
        return s;
    }
    default:
        serFatal("未知的类型标记 0x%02X，数据可能已损坏 / unknown type tag, data may be corrupted", tag);
        return val_none();
    }
}

int lm_deserialize_value_at(Value b, int offset, Value* outVal) {
    if(b.type != VAL_BYTES)
        serFatal("反序列化输入必须是 bytes / deserialize input must be bytes");
    BytesObj* bo = (BytesObj*)b.v.bytes_obj;
    DecCursor c = {bo ? bo->data : NULL, bo ? bo->len : 0, offset};
    if(offset < 0 || offset > c.len)
        serFatal("读取偏移 %d 越界 / read offset out of range", offset);
    Value v = decValue(&c);
    if(outVal) *outVal = v;
    return c.pos;
}

/* ==================== 流头 / 流构建 ==================== */

int lm_check_stream_header(Value b) {
    if(b.type != VAL_BYTES)
        serFatal("反序列化输入必须是 bytes / deserialize input must be bytes");
    BytesObj* bo = (BytesObj*)b.v.bytes_obj;
    int n = bo ? bo->len : 0;
    if(n < 6)
        serFatal("数据过短，不是有效的对象流 / data too short, not a valid object stream");
    const uint8_t* d = bo->data;
    if(d[0] != (uint8_t)SER_MAGIC0 || d[1] != (uint8_t)SER_MAGIC1 ||
       d[2] != (uint8_t)SER_MAGIC2 || d[3] != (uint8_t)SER_MAGIC3)
        serFatal("魔数不匹配，不是有效的对象流 / magic mismatch, not a valid object stream");
    if(d[4] != SER_VERSION)
        serFatal("不支持的流版本 %d（支持版本 %d） / unsupported stream version",
                 (int)d[4], (int)SER_VERSION);
    return 6;
}

Value lm_build_stream(Value chunks) {
    if(chunks.type != VAL_ARRAY)
        serFatal("流构建参数必须是数组 / stream build argument must be an array");
    ValueArray* a = chunks.v.array;
    int chunkN = a ? a->len : 0;
    int total = 6;
    for(int i = 0; i < chunkN; i++) {
        Value c = a->items[i];
        if(c.type != VAL_BYTES)
            serFatal("第 %d 个数据块不是 bytes / chunk %d is not bytes", i + 1, i + 1);
        BytesObj* bo = (BytesObj*)c.v.bytes_obj;
        total += bo ? bo->len : 0;
    }
    uint8_t* out = (uint8_t*)malloc((size_t)(total > 0 ? total : 1));
    if(!out) serFatal("内存不足 / out of memory");
    out[0] = (uint8_t)SER_MAGIC0;
    out[1] = (uint8_t)SER_MAGIC1;
    out[2] = (uint8_t)SER_MAGIC2;
    out[3] = (uint8_t)SER_MAGIC3;
    out[4] = SER_VERSION;
    out[5] = 0;
    int pos = 6;
    for(int i = 0; i < chunkN; i++) {
        BytesObj* bo = (BytesObj*)a->items[i].v.bytes_obj;
        int n = bo ? bo->len : 0;
        if(n > 0) memcpy(out + pos, bo->data, (size_t)n);
        pos += n;
    }
    Value r = lumyr_bytes_from_buf(out, total);
    free(out);
    return r;
}
