// lm_container.c —— 容器与数值扩展类型（tuple/set/bytes/complex）
#include "lm_container.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ===== tuple（VAL_TUPLE）：不可变固定长度异构序列 ===== */

Value lumyr_tuple_make(int argc, const Value* args) {
    TupleObj* o = (TupleObj*)gc_alloc(sizeof(TupleObj), VAL_TUPLE);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->len = argc;
    o->stack_alloc = 0;
    if(argc > 0) {
        o->items = (Value*)gc_alloc(sizeof(Value) * (argc > 0 ? argc : 1), VAL_TUPLE);
        if(!o->items) { o->len = 0; }
        else { memcpy(o->items, args, sizeof(Value) * argc); }
    } else {
        o->items = NULL;
    }
    Value r;
    r.type = VAL_TUPLE;
    r.str_inline = 0;
    r.v.tuple_obj = o;
    return r;
}

int lumyr_tuple_len(Value v) {
    if(v.type != VAL_TUPLE) return 0;
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    return o ? o->len : 0;
}

Value lumyr_tuple_get(Value v, int idx) {
    if(v.type != VAL_TUPLE) return lumyr_make_int(0);
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    if(!o || idx < 0 || idx >= o->len) return lumyr_make_int(0);
    return o->items[idx];
}

_Bool lumyr_tuple_eq(Value a, Value b) {
    if(a.type != VAL_TUPLE || b.type != VAL_TUPLE) return 0;
    TupleObj* oa = (TupleObj*)a.v.tuple_obj;
    TupleObj* ob = (TupleObj*)b.v.tuple_obj;
    if(!oa || !ob) return oa == ob;
    if(oa->len != ob->len) return 0;
    for(int i = 0; i < oa->len; i++) {
        Value e = lumyr_eq(oa->items[i], ob->items[i]);
        if(!lumyr_to_bool(e)) return 0;
    }
    return 1;
}

char* lumyr_tuple_to_str(Value v) {
    if(v.type != VAL_TUPLE) return strdup("()");
    TupleObj* o = (TupleObj*)v.v.tuple_obj;
    if(!o || o->len == 0) return strdup("()");
    // 拼接 (e1, e2, ...)，递归 value_to_str
    size_t cap = 64;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = '(';
    for(int i = 0; i < o->len; i++) {
        if(i > 0) { buf[p++] = ','; buf[p++] = ' '; }
        char* es = value_to_str(o->items[i]);
        if(!es) es = strdup("");
        size_t el = strlen(es);
        while((size_t)p + el + 3 >= cap) {
            cap *= 2;
            buf = (char*)realloc(buf, cap);
        }
        memcpy(buf + p, es, el);
        p += (int)el;
        free(es);
    }
    buf[p++] = ')';
    buf[p] = '\0';
    return buf;
}

/* ===== set（VAL_SET）：无序唯一元素集合（底层 ValueMap） ===== */

static Value set_alloc(void) {
    SetObj* o = (SetObj*)gc_alloc(sizeof(SetObj), VAL_SET);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->map = NULL;
    o->stack_alloc = 0;
    // 底层用空 map
    Value mv = val_map();
    o->map = mv.v.map;
    Value r;
    r.type = VAL_SET;
    r.str_inline = 0;
    r.v.set_obj = o;
    return r;
}

Value lumyr_set_make(int argc, const Value* args) {
    Value r = set_alloc();
    if(r.type != VAL_SET) return r;
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    for(int i = 0; i < argc; i++) {
        lumyr_map_set(&mv, args[i], lumyr_make_int(1));
    }
    return r;
}

int lumyr_set_len(Value v) {
    if(v.type != VAL_SET) return 0;
    SetObj* o = (SetObj*)v.v.set_obj;
    return (o && o->map) ? o->map->len : 0;
}

_Bool lumyr_set_has(Value v, Value elem) {
    if(v.type != VAL_SET) return 0;
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map) return 0;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    return lumyr_map_has(mv, elem) ? 1 : 0;
}

Value lumyr_set_add(Value* v, Value elem) {
    if(!v || v->type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v->v.set_obj;
    if(!o || !o->map) return *v;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    lumyr_map_set(&mv, elem, lumyr_make_int(1));
    return *v;
}

Value lumyr_set_remove(Value* v, Value elem) {
    if(!v || v->type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v->v.set_obj;
    if(!o || !o->map) return *v;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    lumyr_map_del(&mv, elem);
    return *v;
}

Value lumyr_set_to_array(Value v) {
    if(v.type != VAL_SET) return lumyr_make_int(0);
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map) return lumyr_make_int(0);
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    return lumyr_map_keys(mv);
}

static Value set_from_map(ValueMap* m) {
    Value r = set_alloc();
    if(r.type != VAL_SET) return r;
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, m);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

Value lumyr_set_union(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    map_iter_init(&it, ob->map);
    while(map_iter_next(&it, &k, &val)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    return r;
}

Value lumyr_set_intersect(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(lumyr_map_has(mvb, k)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

Value lumyr_set_diff(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return lumyr_make_int(0);
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return lumyr_make_int(0);
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    Value r = set_alloc();
    SetObj* o = (SetObj*)r.v.set_obj;
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(!lumyr_map_has(mvb, k)) lumyr_map_set(&mv, k, lumyr_make_int(1));
    }
    return r;
}

_Bool lumyr_set_eq(Value a, Value b) {
    if(a.type != VAL_SET || b.type != VAL_SET) return 0;
    SetObj* oa = (SetObj*)a.v.set_obj;
    SetObj* ob = (SetObj*)b.v.set_obj;
    if(!oa || !ob) return oa == ob;
    if(oa->map->len != ob->map->len) return 0;
    Value mvb; mvb.type = VAL_MAP; mvb.str_inline = 0; mvb.v.map = ob->map;
    MapIter it;
    map_iter_init(&it, oa->map);
    Value k, val;
    while(map_iter_next(&it, &k, &val)) {
        if(!lumyr_map_has(mvb, k)) return 0;
    }
    return 1;
}

char* lumyr_set_to_str(Value v) {
    if(v.type != VAL_SET) return strdup("{}");
    SetObj* o = (SetObj*)v.v.set_obj;
    if(!o || !o->map || o->map->len == 0) return strdup("{}");
    Value mv; mv.type = VAL_MAP; mv.str_inline = 0; mv.v.map = o->map;
    MapIter it;
    map_iter_init(&it, o->map);
    size_t cap = 64;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = '{';
    Value k, val;
    int first = 1;
    while(map_iter_next(&it, &k, &val)) {
        if(!first) { buf[p++] = ','; buf[p++] = ' '; }
        first = 0;
        char* es = value_to_str(k);
        if(!es) es = strdup("");
        size_t el = strlen(es);
        while((size_t)p + el + 3 >= cap) { cap *= 2; buf = (char*)realloc(buf, cap); }
        memcpy(buf + p, es, el);
        p += (int)el;
        free(es);
    }
    buf[p++] = '}';
    buf[p] = '\0';
    return buf;
}

/* ===== bytes（VAL_BYTES）：不可变字节串 ===== */

static Value bytes_alloc(const uint8_t* data, int len) {
    BytesObj* o = (BytesObj*)gc_alloc(sizeof(BytesObj), VAL_BYTES);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->len = len;
    o->stack_alloc = 0;
    if(len > 0) {
        o->data = (uint8_t*)gc_alloc(sizeof(uint8_t) * (len > 0 ? len : 1), VAL_BYTES);
        if(o->data) memcpy(o->data, data, len);
        else o->len = 0;
    } else {
        o->data = NULL;
    }
    Value r;
    r.type = VAL_BYTES;
    r.str_inline = 0;
    r.v.bytes_obj = o;
    return r;
}

Value lumyr_bytes_from_buf(const uint8_t* data, int len) {
    return bytes_alloc(data, len);
}

Value lumyr_bytes_make(int argc, const Value* args) {
    if(argc == 0) return bytes_alloc(NULL, 0);
    // 单参数：字符串 或 数组
    if(argc == 1) {
        Value a = args[0];
        if(a.type == VAL_STRING) {
            const char* s = lumyr_str_cstr(&a);
            int sl = lumyr_str_len(&a);
            if(!s || sl <= 0) return bytes_alloc(NULL, 0);
            return bytes_alloc((const uint8_t*)s, sl);
        }
        if(a.type == VAL_ARRAY) {
            ValueArray* arr = a.v.array;
            if(!arr || arr->len <= 0) return bytes_alloc(NULL, 0);
            uint8_t* tmp = (uint8_t*)malloc(arr->len);
            for(int i = 0; i < arr->len; i++) {
                tmp[i] = (uint8_t)lumyr_extract_ll(arr->items[i]);
            }
            Value r = bytes_alloc(tmp, arr->len);
            free(tmp);
            return r;
        }
        if(a.type == VAL_TYPED_ARRAY) {
            TypedArray* ta = a.v.typed_array;
            if(!ta || ta->len <= 0) return bytes_alloc(NULL, 0);
            uint8_t* tmp = (uint8_t*)malloc(ta->len);
            // 仅 uint8 元素直接拷贝，其他按整数提取
            if(ta->elem_type == VAL_UINT8 || ta->elem_type == VAL_BYTE || ta->elem_type == VAL_UCHAR) {
                memcpy(tmp, ta->items, ta->len);
            } else {
                for(int i = 0; i < ta->len; i++) {
                    tmp[i] = (uint8_t)lumyr_extract_ll(((Value*)ta->items)[i]);
                }
            }
            Value r = bytes_alloc(tmp, ta->len);
            free(tmp);
            return r;
        }
    }
    // 多参数：按整数提取
    uint8_t* tmp = (uint8_t*)malloc(argc > 0 ? argc : 1);
    for(int i = 0; i < argc; i++) tmp[i] = (uint8_t)lumyr_extract_ll(args[i]);
    Value r = bytes_alloc(tmp, argc);
    free(tmp);
    return r;
}

Value lumyr_bytes_from_hex(const char* hex) {
    if(!hex) return bytes_alloc(NULL, 0);
    int hl = (int)strlen(hex);
    if(hl % 2 != 0) return bytes_alloc(NULL, 0);
    int bl = hl / 2;
    if(bl <= 0) return bytes_alloc(NULL, 0);
    uint8_t* tmp = (uint8_t*)malloc(bl);
    for(int i = 0; i < bl; i++) {
        char h1 = hex[i*2], h2 = hex[i*2+1];
        int v1 = -1, v2 = -1;
        if(h1 >= '0' && h1 <= '9') v1 = h1 - '0';
        else if(h1 >= 'a' && h1 <= 'f') v1 = h1 - 'a' + 10;
        else if(h1 >= 'A' && h1 <= 'F') v1 = h1 - 'A' + 10;
        if(h2 >= '0' && h2 <= '9') v2 = h2 - '0';
        else if(h2 >= 'a' && h2 <= 'f') v2 = h2 - 'a' + 10;
        else if(h2 >= 'A' && h2 <= 'F') v2 = h2 - 'A' + 10;
        if(v1 < 0 || v2 < 0) { free(tmp); return bytes_alloc(NULL, 0); }
        tmp[i] = (uint8_t)((v1 << 4) | v2);
    }
    Value r = bytes_alloc(tmp, bl);
    free(tmp);
    return r;
}

int lumyr_bytes_len(Value v) {
    if(v.type != VAL_BYTES) return 0;
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    return o ? o->len : 0;
}

Value lumyr_bytes_get(Value v, int idx) {
    if(v.type != VAL_BYTES) return lumyr_make_int(0);
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || idx < 0 || idx >= o->len) return lumyr_make_int(0);
    return lumyr_make_int((int)o->data[idx]);
}

char* lumyr_bytes_to_str(Value v) {
    if(v.type != VAL_BYTES) return strdup("b\"\"");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("b\"\"");
    // 输出 b"..."，可打印字符原样，不可打印用 \xNN
    size_t cap = (size_t)o->len * 4 + 8;
    char* buf = (char*)malloc(cap);
    int p = 0;
    buf[p++] = 'b';
    buf[p++] = '"';
    for(int i = 0; i < o->len; i++) {
        uint8_t c = o->data[i];
        if(c >= 0x20 && c < 0x7f && c != '"' && c != '\\') {
            buf[p++] = (char)c;
        } else {
            buf[p++] = '\\';
            if(c == '"') buf[p++] = '"';
            else if(c == '\\') buf[p++] = '\\';
            else if(c == '\n') { buf[p++] = 'n'; }
            else if(c == '\t') { buf[p++] = 't'; }
            else if(c == '\r') { buf[p++] = 'r'; }
            else {
                buf[p++] = 'x';
                const char* hx = "0123456789abcdef";
                buf[p++] = hx[(c >> 4) & 0xf];
                buf[p++] = hx[c & 0xf];
            }
        }
    }
    buf[p++] = '"';
    buf[p] = '\0';
    return buf;
}

char* lumyr_bytes_hex(Value v) {
    if(v.type != VAL_BYTES) return strdup("");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("");
    char* buf = (char*)malloc((size_t)o->len * 2 + 1);
    const char* hx = "0123456789abcdef";
    for(int i = 0; i < o->len; i++) {
        buf[i*2] = hx[(o->data[i] >> 4) & 0xf];
        buf[i*2+1] = hx[o->data[i] & 0xf];
    }
    buf[o->len * 2] = '\0';
    return buf;
}

char* lumyr_bytes_to_utf8(Value v) {
    if(v.type != VAL_BYTES) return strdup("");
    BytesObj* o = (BytesObj*)v.v.bytes_obj;
    if(!o || o->len <= 0) return strdup("");
    char* buf = (char*)malloc(o->len + 1);
    memcpy(buf, o->data, o->len);
    buf[o->len] = '\0';
    return buf;
}

/* ===== complex（VAL_COMPLEX）：复数 real+imag double ===== */

Value lumyr_complex_make(double real, double imag) {
    ComplexObj* o = (ComplexObj*)gc_alloc(sizeof(ComplexObj), VAL_COMPLEX);
    if(!o) {
        Value z; z.type = VAL_NONE; z.str_inline = 0; return z;
    }
    o->real = real;
    o->imag = imag;
    Value r;
    r.type = VAL_COMPLEX;
    r.str_inline = 0;
    r.v.complex_obj = o;
    return r;
}

double lumyr_complex_real(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    return o ? o->real : 0.0;
}

double lumyr_complex_imag(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    return o ? o->imag : 0.0;
}

double lumyr_complex_abs(Value v) {
    if(v.type != VAL_COMPLEX) return 0.0;
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return 0.0;
    return sqrt(o->real * o->real + o->imag * o->imag);
}

Value lumyr_complex_conjugate(Value v) {
    if(v.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return lumyr_make_double(0.0);
    return lumyr_complex_make(o->real, -o->imag);
}

char* lumyr_complex_to_str(Value v) {
    if(v.type != VAL_COMPLEX) return strdup("(0+0j)");
    ComplexObj* o = (ComplexObj*)v.v.complex_obj;
    if(!o) return strdup("(0+0j)");
    char buf[128];
    if(o->imag >= 0) {
        snprintf(buf, sizeof(buf), "(%g+%gj)", o->real, o->imag);
    } else {
        snprintf(buf, sizeof(buf), "(%g%gj)", o->real, o->imag);
    }
    return strdup(buf);
}

Value lumyr_complex_add(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    return lumyr_complex_make(oa->real + ob->real, oa->imag + ob->imag);
}

Value lumyr_complex_sub(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    return lumyr_complex_make(oa->real - ob->real, oa->imag - ob->imag);
}

Value lumyr_complex_mul(Value a, Value b) {
    if(a.type != VAL_COMPLEX || b.type != VAL_COMPLEX) return lumyr_make_double(0.0);
    ComplexObj* oa = (ComplexObj*)a.v.complex_obj;
    ComplexObj* ob = (ComplexObj*)b.v.complex_obj;
    if(!oa || !ob) return lumyr_make_double(0.0);
    // (a+bi)(c+di) = (ac-bd) + (ad+bc)i
    double r = oa->real * ob->real - oa->imag * ob->imag;
    double i = oa->real * ob->imag + oa->imag * ob->real;
    return lumyr_complex_make(r, i);
}
