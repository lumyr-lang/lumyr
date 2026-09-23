// lm_formdata.c —— formdata 对象（VAL_FORMDATA）实现
#include "lm_formdata.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FD_DEFAULT_CAP 4

Value lumyr_formdata_make(int cap)
{
    Value r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_FORMDATA;
    if(cap < 1) cap = FD_DEFAULT_CAP;

    gc_disable();
    FormDataObj* o = (FormDataObj*)gc_alloc(sizeof(FormDataObj), VAL_FORMDATA);
    o->names = (char**)gc_alloc(sizeof(char*) * (size_t)cap, VAL_FORMDATA);
    o->vals  = (Value*)gc_alloc(sizeof(Value) * (size_t)cap, VAL_FORMDATA);
    gc_mark_internal_buf(o->names);
    gc_mark_internal_buf(o->vals);
    o->len = 0;
    o->cap = cap;
    o->stack_alloc = 0;
    r.v.formdata_obj = o;
    gc_enable();
    return r;
}

int lumyr_formdata_add(Value fd, Value nameVal, Value val)
{
    if(fd.type != VAL_FORMDATA || !fd.v.formdata_obj) return 0;
    FormDataObj* o = (FormDataObj*)fd.v.formdata_obj;

    char* nm = value_to_str(nameVal);
    if(!nm) return 0;
    size_t nl = strlen(nm);

    gc_disable();
    if(o->len >= o->cap) {
        int ncap2 = o->cap * 2;
        char** nn = (char**)gc_realloc(o->names, sizeof(char*) * (size_t)ncap2);
        Value*  nv = (Value*)gc_realloc(o->vals, sizeof(Value) * (size_t)ncap2);
        if(!nn || !nv) { gc_enable(); free(nm); return 0; }
        o->names = nn;
        o->vals  = nv;
        o->cap = ncap2;
        gc_mark_internal_buf(o->names);
        gc_mark_internal_buf(o->vals);
    }
    char* nameCopy = (char*)gc_alloc(nl + 1, VAL_FORMDATA);
    if(!nameCopy) { gc_enable(); free(nm); return 0; }
    memcpy(nameCopy, nm, nl + 1);
    o->names[o->len] = nameCopy;
    o->vals[o->len] = val;
    o->len++;
    gc_enable();
    free(nm);
    return 1;
}

int lumyr_formdata_set(Value fd, Value nameVal, Value val)
{
    if(fd.type != VAL_FORMDATA || !fd.v.formdata_obj) return 0;
    FormDataObj* o = (FormDataObj*)fd.v.formdata_obj;

    char* nm = value_to_str(nameVal);
    if(!nm) return 0;

    /* 原地压缩删除全部同名字段（保持其余字段相对顺序），再追加 */
    int w = 0;
    for(int i = 0; i < o->len; i++) {
        if(strcmp(o->names[i], nm) == 0) continue;
        o->names[w] = o->names[i];
        o->vals[w] = o->vals[i];
        w++;
    }
    o->len = w;
    free(nm);
    return lumyr_formdata_add(fd, nameVal, val);
}

Value lumyr_formdata_get_by_name(Value fd, Value nameVal)
{
    if(fd.type != VAL_FORMDATA || !fd.v.formdata_obj) return val_none();
    FormDataObj* o = (FormDataObj*)fd.v.formdata_obj;
    char* nm = value_to_str(nameVal);
    if(!nm) return val_none();
    Value r = val_none();
    for(int i = 0; i < o->len; i++) {
        if(strcmp(o->names[i], nm) == 0) { r = o->vals[i]; break; }
    }
    free(nm);
    return r;
}

int lumyr_formdata_len(Value v)
{
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return 0;
    return ((FormDataObj*)v.v.formdata_obj)->len;
}

const char* lumyr_formdata_name(Value v, int i)
{
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return NULL;
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
    if(i < 0 || i >= o->len) return NULL;
    return o->names[i];
}

Value lumyr_formdata_get(Value v, int i)
{
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return val_none();
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
    if(i < 0 || i >= o->len) return val_none();
    return o->vals[i];
}

Value lumyr_formdata_keys(Value v)
{
    int n = lumyr_formdata_len(v);
    Value r = val_array(n);
    if(n > 0) {
        FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
        for(int i = 0; i < n; i++)
            r.v.array->items[i] = lumyr_make_string(o->names[i] ? o->names[i] : "");
    }
    return r;
}

Value lumyr_formdata_values(Value v)
{
    int n = lumyr_formdata_len(v);
    Value r = val_array(n);
    if(n > 0) {
        FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
        for(int i = 0; i < n; i++)
            r.v.array->items[i] = o->vals[i];
    }
    return r;
}

Value lumyr_formdata_get_all(Value v, Value nameVal)
{
    Value r = val_array(0);
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return r;
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
    char* nm = value_to_str(nameVal);
    if(!nm) return r;
    int n = 0; /* 匹配数 */
    for(int i = 0; i < o->len; i++) {
        if(strcmp(o->names[i], nm) != 0) continue;
        if(n > 0) {
            /* 扩容快照数组，逐个追加（避免回调期间持有内部指针） */
            Value nr = val_array(n + 1);
            for(int j = 0; j < n; j++) nr.v.array->items[j] = r.v.array->items[j];
            nr.v.array->items[n] = o->vals[i];
            r = nr;
        } else {
            r = val_array(1);
            r.v.array->items[0] = o->vals[i];
        }
        n++;
    }
    free(nm);
    return r;
}

int lumyr_formdata_has(Value v, Value nameVal)
{
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return 0;
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
    char* nm = value_to_str(nameVal);
    if(!nm) return 0;
    int found = 0;
    for(int i = 0; i < o->len; i++) {
        if(strcmp(o->names[i], nm) == 0) { found = 1; break; }
    }
    free(nm);
    return found;
}

int lumyr_formdata_delete(Value v, Value nameVal)
{
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return 0;
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;
    char* nm = value_to_str(nameVal);
    if(!nm) return 0;
    /* 原地压缩：跳过全部同名字段，其余保持相对顺序 */
    int w = 0, removed = 0;
    for(int i = 0; i < o->len; i++) {
        if(strcmp(o->names[i], nm) == 0) { removed++; continue; }
        o->names[w] = o->names[i];
        o->vals[w] = o->vals[i];
        w++;
    }
    o->len = w;
    free(nm);
    return removed > 0 ? 1 : 0;
}

Value lumyr_formdata_field(Value v, const char* name)
{
    if(strcmp(name, "len") == 0)
        return lumyr_make_int(lumyr_formdata_len(v));
    return val_none();
}

/* ---------- 字符串化 ---------- */

typedef struct { char* data; int len; int cap; } SB;

static int sb_reserve(SB* b, int extra)
{
    if(b->len + extra + 1 <= b->cap) return 1;
    int nc = b->cap ? b->cap * 2 : 64;
    while(nc < b->len + extra + 1) nc *= 2;
    char* p = (char*)realloc(b->data, (size_t)nc);
    if(!p) return 0;
    b->data = p; b->cap = nc;
    return 1;
}

static int sb_puts(SB* b, const char* s)
{
    int n = (int)strlen(s);
    if(!sb_reserve(b, n)) return 0;
    memcpy(b->data + b->len, s, (size_t)n);
    b->len += n;
    return 1;
}

static int sb_putc(SB* b, char c)
{
    if(!sb_reserve(b, 1)) return 0;
    b->data[b->len++] = c;
    return 1;
}

char* lumyr_formdata_to_str(Value v)
{
    SB b;
    b.data = NULL; b.len = 0; b.cap = 0;
    if(v.type != VAL_FORMDATA || !v.v.formdata_obj) return strdup("formdata {}");
    FormDataObj* o = (FormDataObj*)v.v.formdata_obj;

    sb_puts(&b, "formdata {");
    for(int i = 0; i < o->len; i++) {
        if(i > 0) sb_putc(&b, ',');
        sb_putc(&b, ' ');
        sb_puts(&b, o->names[i]);
        sb_puts(&b, ": ");
        Value ev = o->vals[i];
        if(ev.type == VAL_FILE) {
            const char* path = ((FileObj*)ev.v.file_obj)->path;
            char tmp[300];
            snprintf(tmp, sizeof(tmp), "<file \"%s\">", path ? path : "");
            sb_puts(&b, tmp);
        } else if(ev.type == VAL_BYTES) {
            sb_puts(&b, "<bytes>");
        } else {
            char* s = value_to_str(ev);
            sb_puts(&b, s ? s : "");
            free(s);
        }
    }
    if(o->len > 0) sb_putc(&b, ' ');
    sb_putc(&b, '}');
    sb_reserve(&b, 1);
    b.data[b.len] = '\0';
    return b.data;
}
