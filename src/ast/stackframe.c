#include "stackframe.h"
#include "lumyr_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 释放一个帧内槽位的资源。
// 函数值（VAL_FUNC）是引用语义：帧不拥有 RuntimeFunc，销毁会误伤共享对象，
// 因此跳过，只清空类型标记。
static void slot_release(Value* v) {
    if(!v) return;
    if(v->type == VAL_FUNC) {
        v->type = VAL_NONE;
        return;
    }
    val_destroy(v);
}

StackFrame* stackframe_new(StackFrame* parent)
{
    StackFrame* f = (StackFrame*)calloc(1, sizeof(StackFrame));
    if(!f) {
        perror("stackframe_new");
        exit(EXIT_FAILURE);
    }
    f->cnt = 0;
    f->cap = 0;
    f->names = NULL;
    f->vals = NULL;
    f->parent = parent;
    f->shared = 0;
    f->cell_names = NULL;
    f->cells = NULL;
    f->cell_cnt = 0;
    f->cell_cap = 0;
    pthread_rwlock_init(&f->rw, NULL);
    return f;
}

void stackframe_set_shared(StackFrame* f)
{
    if(f) f->shared = 1;
}

/* 帧内变量槽扩容：翻倍，无硬上限。调用方必须已持有该帧的写锁（若 shared）
 *
 * 注意：使用 malloc + memcpy 而非 realloc，因为 realloc 可能释放旧缓冲区，
 * 而 f->vals/f->names 指针在 realloc 返回后才更新。在这个窗口内，另一个线程
 * 的 GC 可能扫描该帧并读到已释放的旧指针 → UAF → segfault。
 * 改用 malloc + memcpy 后，先更新指针再 free 旧缓冲区，GC 永远不会读到已释放指针。 */
static void frame_ensure(StackFrame* f, int need)
{
    if(need <= f->cap) return;
    int newcap = f->cap > 0 ? f->cap : 16;
    while(newcap < need) newcap *= 2;

    /* 扩容 names：malloc + memcpy，更新指针后 free 旧缓冲区 */
    char** nn = (char**)malloc((size_t)newcap * sizeof(char*));
    if(!nn) { perror("stackframe expand names"); exit(EXIT_FAILURE); }
    if(f->names) {
        memcpy(nn, f->names, (size_t)f->cap * sizeof(char*));
    }
    /* 新槽位初始化为 NULL */
    for(int i = f->cap; i < newcap; i++) nn[i] = NULL;
    char** old_names = f->names;
    f->names = nn;
    free(old_names);

    /* 扩容 vals：malloc + memcpy，更新指针后 free 旧缓冲区 */
    Value* nv = (Value*)malloc((size_t)newcap * sizeof(Value));
    if(!nv) { perror("stackframe expand vals"); exit(EXIT_FAILURE); }
    if(f->vals) {
        memcpy(nv, f->vals, (size_t)f->cap * sizeof(Value));
    }
    /* 新槽位初始化为 VAL_NONE */
    for(int i = f->cap; i < newcap; i++) {
        nv[i].type = VAL_NONE;
        nv[i].v.i = 0;
    }
    Value* old_vals = f->vals;
    f->vals = nv;
    free(old_vals);

    /* 扩容 type_tags：malloc + memcpy，更新指针后 free 旧缓冲区 */
    int* nt = (int*)malloc((size_t)newcap * sizeof(int));
    if(!nt) { perror("stackframe expand type_tags"); exit(EXIT_FAILURE); }
    if(f->type_tags) {
        memcpy(nt, f->type_tags, (size_t)f->cap * sizeof(int));
    }
    /* 新槽位初始化为 -1（无精确类型） */
    for(int i = f->cap; i < newcap; i++) nt[i] = -1;
    int* old_tags = f->type_tags;
    f->type_tags = nt;
    free(old_tags);

    /* 扩容 int_vals */
    int* nint_vals = (int*)malloc((size_t)newcap * sizeof(int));
    if(!nint_vals) { perror("stackframe expand int_vals"); exit(EXIT_FAILURE); }
    if(f->int_vals) memcpy(nint_vals, f->int_vals, (size_t)f->cap * sizeof(int));
    for(int i = f->cap; i < newcap; i++) nint_vals[i] = (int)0;
    int* old_int_vals = f->int_vals;
    f->int_vals = nint_vals;
    free(old_int_vals);
    /* 扩容 longlong_vals */
    long long* nlonglong_vals = (long long*)malloc((size_t)newcap * sizeof(long long));
    if(!nlonglong_vals) { perror("stackframe expand longlong_vals"); exit(EXIT_FAILURE); }
    if(f->longlong_vals) memcpy(nlonglong_vals, f->longlong_vals, (size_t)f->cap * sizeof(long long));
    for(int i = f->cap; i < newcap; i++) nlonglong_vals[i] = (long long)0;
    long long* old_longlong_vals = f->longlong_vals;
    f->longlong_vals = nlonglong_vals;
    free(old_longlong_vals);
    /* 扩容 long_vals */
    long* nlong_vals = (long*)malloc((size_t)newcap * sizeof(long));
    if(!nlong_vals) { perror("stackframe expand long_vals"); exit(EXIT_FAILURE); }
    if(f->long_vals) memcpy(nlong_vals, f->long_vals, (size_t)f->cap * sizeof(long));
    for(int i = f->cap; i < newcap; i++) nlong_vals[i] = (long)0;
    long* old_long_vals = f->long_vals;
    f->long_vals = nlong_vals;
    free(old_long_vals);
    /* 扩容 short_vals */
    short* nshort_vals = (short*)malloc((size_t)newcap * sizeof(short));
    if(!nshort_vals) { perror("stackframe expand short_vals"); exit(EXIT_FAILURE); }
    if(f->short_vals) memcpy(nshort_vals, f->short_vals, (size_t)f->cap * sizeof(short));
    for(int i = f->cap; i < newcap; i++) nshort_vals[i] = (short)0;
    short* old_short_vals = f->short_vals;
    f->short_vals = nshort_vals;
    free(old_short_vals);
    /* 扩容 int8_vals */
    int8_t* nint8_vals = (int8_t*)malloc((size_t)newcap * sizeof(int8_t));
    if(!nint8_vals) { perror("stackframe expand int8_vals"); exit(EXIT_FAILURE); }
    if(f->int8_vals) memcpy(nint8_vals, f->int8_vals, (size_t)f->cap * sizeof(int8_t));
    for(int i = f->cap; i < newcap; i++) nint8_vals[i] = (int8_t)0;
    int8_t* old_int8_vals = f->int8_vals;
    f->int8_vals = nint8_vals;
    free(old_int8_vals);
    /* 扩容 int16_vals */
    int16_t* nint16_vals = (int16_t*)malloc((size_t)newcap * sizeof(int16_t));
    if(!nint16_vals) { perror("stackframe expand int16_vals"); exit(EXIT_FAILURE); }
    if(f->int16_vals) memcpy(nint16_vals, f->int16_vals, (size_t)f->cap * sizeof(int16_t));
    for(int i = f->cap; i < newcap; i++) nint16_vals[i] = (int16_t)0;
    int16_t* old_int16_vals = f->int16_vals;
    f->int16_vals = nint16_vals;
    free(old_int16_vals);
    /* 扩容 int32_vals */
    int32_t* nint32_vals = (int32_t*)malloc((size_t)newcap * sizeof(int32_t));
    if(!nint32_vals) { perror("stackframe expand int32_vals"); exit(EXIT_FAILURE); }
    if(f->int32_vals) memcpy(nint32_vals, f->int32_vals, (size_t)f->cap * sizeof(int32_t));
    for(int i = f->cap; i < newcap; i++) nint32_vals[i] = (int32_t)0;
    int32_t* old_int32_vals = f->int32_vals;
    f->int32_vals = nint32_vals;
    free(old_int32_vals);
    /* 扩容 int64_vals */
    int64_t* nint64_vals = (int64_t*)malloc((size_t)newcap * sizeof(int64_t));
    if(!nint64_vals) { perror("stackframe expand int64_vals"); exit(EXIT_FAILURE); }
    if(f->int64_vals) memcpy(nint64_vals, f->int64_vals, (size_t)f->cap * sizeof(int64_t));
    for(int i = f->cap; i < newcap; i++) nint64_vals[i] = (int64_t)0;
    int64_t* old_int64_vals = f->int64_vals;
    f->int64_vals = nint64_vals;
    free(old_int64_vals);
    /* 扩容 uint8_vals */
    uint8_t* nuint8_vals = (uint8_t*)malloc((size_t)newcap * sizeof(uint8_t));
    if(!nuint8_vals) { perror("stackframe expand uint8_vals"); exit(EXIT_FAILURE); }
    if(f->uint8_vals) memcpy(nuint8_vals, f->uint8_vals, (size_t)f->cap * sizeof(uint8_t));
    for(int i = f->cap; i < newcap; i++) nuint8_vals[i] = (uint8_t)0;
    uint8_t* old_uint8_vals = f->uint8_vals;
    f->uint8_vals = nuint8_vals;
    free(old_uint8_vals);
    /* 扩容 uint16_vals */
    uint16_t* nuint16_vals = (uint16_t*)malloc((size_t)newcap * sizeof(uint16_t));
    if(!nuint16_vals) { perror("stackframe expand uint16_vals"); exit(EXIT_FAILURE); }
    if(f->uint16_vals) memcpy(nuint16_vals, f->uint16_vals, (size_t)f->cap * sizeof(uint16_t));
    for(int i = f->cap; i < newcap; i++) nuint16_vals[i] = (uint16_t)0;
    uint16_t* old_uint16_vals = f->uint16_vals;
    f->uint16_vals = nuint16_vals;
    free(old_uint16_vals);
    /* 扩容 uint32_vals */
    uint32_t* nuint32_vals = (uint32_t*)malloc((size_t)newcap * sizeof(uint32_t));
    if(!nuint32_vals) { perror("stackframe expand uint32_vals"); exit(EXIT_FAILURE); }
    if(f->uint32_vals) memcpy(nuint32_vals, f->uint32_vals, (size_t)f->cap * sizeof(uint32_t));
    for(int i = f->cap; i < newcap; i++) nuint32_vals[i] = (uint32_t)0;
    uint32_t* old_uint32_vals = f->uint32_vals;
    f->uint32_vals = nuint32_vals;
    free(old_uint32_vals);
    /* 扩容 uint64_vals */
    uint64_t* nuint64_vals = (uint64_t*)malloc((size_t)newcap * sizeof(uint64_t));
    if(!nuint64_vals) { perror("stackframe expand uint64_vals"); exit(EXIT_FAILURE); }
    if(f->uint64_vals) memcpy(nuint64_vals, f->uint64_vals, (size_t)f->cap * sizeof(uint64_t));
    for(int i = f->cap; i < newcap; i++) nuint64_vals[i] = (uint64_t)0;
    uint64_t* old_uint64_vals = f->uint64_vals;
    f->uint64_vals = nuint64_vals;
    free(old_uint64_vals);
    /* 扩容 uchar_vals */
    unsigned char* nuchar_vals = (unsigned char*)malloc((size_t)newcap * sizeof(unsigned char));
    if(!nuchar_vals) { perror("stackframe expand uchar_vals"); exit(EXIT_FAILURE); }
    if(f->uchar_vals) memcpy(nuchar_vals, f->uchar_vals, (size_t)f->cap * sizeof(unsigned char));
    for(int i = f->cap; i < newcap; i++) nuchar_vals[i] = (unsigned char)0;
    unsigned char* old_uchar_vals = f->uchar_vals;
    f->uchar_vals = nuchar_vals;
    free(old_uchar_vals);
    /* 扩容 ushort_vals */
    unsigned short* nushort_vals = (unsigned short*)malloc((size_t)newcap * sizeof(unsigned short));
    if(!nushort_vals) { perror("stackframe expand ushort_vals"); exit(EXIT_FAILURE); }
    if(f->ushort_vals) memcpy(nushort_vals, f->ushort_vals, (size_t)f->cap * sizeof(unsigned short));
    for(int i = f->cap; i < newcap; i++) nushort_vals[i] = (unsigned short)0;
    unsigned short* old_ushort_vals = f->ushort_vals;
    f->ushort_vals = nushort_vals;
    free(old_ushort_vals);
    /* 扩容 ulong_vals */
    unsigned long* nulong_vals = (unsigned long*)malloc((size_t)newcap * sizeof(unsigned long));
    if(!nulong_vals) { perror("stackframe expand ulong_vals"); exit(EXIT_FAILURE); }
    if(f->ulong_vals) memcpy(nulong_vals, f->ulong_vals, (size_t)f->cap * sizeof(unsigned long));
    for(int i = f->cap; i < newcap; i++) nulong_vals[i] = (unsigned long)0;
    unsigned long* old_ulong_vals = f->ulong_vals;
    f->ulong_vals = nulong_vals;
    free(old_ulong_vals);
    /* 扩容 size_t_vals */
    size_t* nsize_t_vals = (size_t*)malloc((size_t)newcap * sizeof(size_t));
    if(!nsize_t_vals) { perror("stackframe expand size_t_vals"); exit(EXIT_FAILURE); }
    if(f->size_t_vals) memcpy(nsize_t_vals, f->size_t_vals, (size_t)f->cap * sizeof(size_t));
    for(int i = f->cap; i < newcap; i++) nsize_t_vals[i] = (size_t)0;
    size_t* old_size_t_vals = f->size_t_vals;
    f->size_t_vals = nsize_t_vals;
    free(old_size_t_vals);
    /* 扩容 ssize_t_vals */
    ssize_t* nssize_t_vals = (ssize_t*)malloc((size_t)newcap * sizeof(ssize_t));
    if(!nssize_t_vals) { perror("stackframe expand ssize_t_vals"); exit(EXIT_FAILURE); }
    if(f->ssize_t_vals) memcpy(nssize_t_vals, f->ssize_t_vals, (size_t)f->cap * sizeof(ssize_t));
    for(int i = f->cap; i < newcap; i++) nssize_t_vals[i] = (ssize_t)0;
    ssize_t* old_ssize_t_vals = f->ssize_t_vals;
    f->ssize_t_vals = nssize_t_vals;
    free(old_ssize_t_vals);
    /* 扩容 float_vals */
    float* nfloat_vals = (float*)malloc((size_t)newcap * sizeof(float));
    if(!nfloat_vals) { perror("stackframe expand float_vals"); exit(EXIT_FAILURE); }
    if(f->float_vals) memcpy(nfloat_vals, f->float_vals, (size_t)f->cap * sizeof(float));
    for(int i = f->cap; i < newcap; i++) nfloat_vals[i] = (float)0.0f;
    float* old_float_vals = f->float_vals;
    f->float_vals = nfloat_vals;
    free(old_float_vals);
    /* 扩容 double_vals */
    double* ndouble_vals = (double*)malloc((size_t)newcap * sizeof(double));
    if(!ndouble_vals) { perror("stackframe expand double_vals"); exit(EXIT_FAILURE); }
    if(f->double_vals) memcpy(ndouble_vals, f->double_vals, (size_t)f->cap * sizeof(double));
    for(int i = f->cap; i < newcap; i++) ndouble_vals[i] = (double)0.0;
    double* old_double_vals = f->double_vals;
    f->double_vals = ndouble_vals;
    free(old_double_vals);
    /* 扩容 longdouble_vals */
    long double* nlongdouble_vals = (long double*)malloc((size_t)newcap * sizeof(long double));
    if(!nlongdouble_vals) { perror("stackframe expand longdouble_vals"); exit(EXIT_FAILURE); }
    if(f->longdouble_vals) memcpy(nlongdouble_vals, f->longdouble_vals, (size_t)f->cap * sizeof(long double));
    for(int i = f->cap; i < newcap; i++) nlongdouble_vals[i] = (long double)0.0L;
    long double* old_longdouble_vals = f->longdouble_vals;
    f->longdouble_vals = nlongdouble_vals;
    free(old_longdouble_vals);
    /* 扩容 bool_vals */
    _Bool* nbool_vals = (_Bool*)malloc((size_t)newcap * sizeof(_Bool));
    if(!nbool_vals) { perror("stackframe expand bool_vals"); exit(EXIT_FAILURE); }
    if(f->bool_vals) memcpy(nbool_vals, f->bool_vals, (size_t)f->cap * sizeof(_Bool));
    for(int i = f->cap; i < newcap; i++) nbool_vals[i] = (_Bool)0;
    _Bool* old_bool_vals = f->bool_vals;
    f->bool_vals = nbool_vals;
    free(old_bool_vals);
    /* 扩容 char_vals */
    char* nchar_vals = (char*)malloc((size_t)newcap * sizeof(char));
    if(!nchar_vals) { perror("stackframe expand char_vals"); exit(EXIT_FAILURE); }
    if(f->char_vals) memcpy(nchar_vals, f->char_vals, (size_t)f->cap * sizeof(char));
    for(int i = f->cap; i < newcap; i++) nchar_vals[i] = (char)0;
    char* old_char_vals = f->char_vals;
    f->char_vals = nchar_vals;
    free(old_char_vals);
    /* 扩容 byte_vals */
    unsigned char* nbyte_vals = (unsigned char*)malloc((size_t)newcap * sizeof(unsigned char));
    if(!nbyte_vals) { perror("stackframe expand byte_vals"); exit(EXIT_FAILURE); }
    if(f->byte_vals) memcpy(nbyte_vals, f->byte_vals, (size_t)f->cap * sizeof(unsigned char));
    for(int i = f->cap; i < newcap; i++) nbyte_vals[i] = (unsigned char)0;
    unsigned char* old_byte_vals = f->byte_vals;
    f->byte_vals = nbyte_vals;
    free(old_byte_vals);
    /* 扩容 string_vals */
    char** nstring_vals = (char**)malloc((size_t)newcap * sizeof(char*));
    if(!nstring_vals) { perror("stackframe expand string_vals"); exit(EXIT_FAILURE); }
    if(f->string_vals) memcpy(nstring_vals, f->string_vals, (size_t)f->cap * sizeof(char*));
    for(int i = f->cap; i < newcap; i++) nstring_vals[i] = NULL;
    char** old_string_vals = f->string_vals;
    f->string_vals = nstring_vals;
    free(old_string_vals);
    /* 扩容 ptr_vals */
    void** nptr_vals = (void**)malloc((size_t)newcap * sizeof(void*));
    if(!nptr_vals) { perror("stackframe expand ptr_vals"); exit(EXIT_FAILURE); }
    if(f->ptr_vals) memcpy(nptr_vals, f->ptr_vals, (size_t)f->cap * sizeof(void*));
    for(int i = f->cap; i < newcap; i++) nptr_vals[i] = NULL;
    void** old_ptr_vals = f->ptr_vals;
    f->ptr_vals = nptr_vals;
    free(old_ptr_vals);

    f->cap = newcap;
}

void stackframe_destroy(StackFrame* f)
{
    if(!f) return;
    for(int i = 0; i < f->cnt; i++) {
        free(f->names[i]);
        slot_release(&f->vals[i]);
    }
    free(f->names);
    free(f->vals);
    /* 基础整数类型 */
    free(f->int_vals);
    free(f->longlong_vals);
    free(f->long_vals);
    free(f->short_vals);
    /* 固定宽度有符号整数 */
    free(f->int8_vals);
    free(f->int16_vals);
    free(f->int32_vals);
    free(f->int64_vals);
    /* 固定宽度无符号整数 */
    free(f->uint8_vals);
    free(f->uint16_vals);
    free(f->uint32_vals);
    free(f->uint64_vals);
    /* 其他无符号整数 */
    free(f->uchar_vals);
    free(f->ushort_vals);
    free(f->ulong_vals);
    free(f->size_t_vals);
    free(f->ssize_t_vals);
    /* 浮点类型 */
    free(f->float_vals);
    free(f->double_vals);
    free(f->longdouble_vals);
    /* 其他基础类型 */
    free(f->bool_vals);
    free(f->char_vals);
    free(f->byte_vals);
    free(f->string_vals);
    free(f->ptr_vals);
    /* cell 表：cell 指针本身由闭包持有，这里只释放表项名与指针数组 */
    for(int i = 0; i < f->cell_cnt; i++) {
        free(f->cell_names[i]);
    }
    free(f->cell_names);
    free(f->cells);
    free(f->type_tags);
    pthread_rwlock_destroy(&f->rw);
    free(f);
}

// 只查当前帧 cell 表（调用方须已持锁）
static int find_cell_in_frame(StackFrame* f, const char* name)
{
    for(int i = 0; i < f->cell_cnt; i++) {
        if(strcmp(f->cell_names[i], name) == 0) return i;
    }
    return -1;
}

// 只查当前帧（调用方必须已持有该帧锁，若 shared）
static int find_in_frame(StackFrame* f, const char* name)
{
    for(int i = 0; i < f->cnt; i++) {
        if(strcmp(f->names[i], name) == 0) return i;
    }
    return -1;
}

Value stackframe_get(StackFrame* f, const char* name, _Bool* found)
{
    Value zero;
    memset(&zero, 0, sizeof(zero));
    if(found) *found = 0;
    if(!f || !name) return zero;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        /* cell 优先：被捕获变量经堆单元间接访问（引用语义） */
        int ci = find_cell_in_frame(p, name);
        if(ci >= 0) {
            Value v = *(p->cells[ci]);
            if(hl) pthread_rwlock_unlock(&p->rw);
            if(found) *found = 1;
            return v;
        }
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                Value v = p->vals[i];   // 锁内拷贝（浅拷贝，语义与旧实现一致）
                if(hl) pthread_rwlock_unlock(&p->rw);
                if(found) *found = 1;
                return v;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return zero;
}

/* 设置变量的类型标记（CastKind 枚举，-1 表示无精确类型）
   当设置类型标记为 CAST_INT 时，同时从 vals 提取 int 值存储到 int_vals，
   用于 OPC_LOAD_INT_VAR 零提取 */
void stackframe_set_type_tag(StackFrame* f, const char* name, int type_tag)
{
    if(!f || !name) return;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_wrlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                if(p->type_tags) p->type_tags[i] = type_tag;
                /* 当设置为 int 类型时，同时更新 int_vals */
                if(type_tag == 2 /* CAST_INT */ && p->int_vals) {
                    Value v = p->vals[i];
                    int iv = 0;
                    switch(v.type) {
                        case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                            iv = (int)v.v.i; break;
                        case 2:  // VAL_DOUBLE
                            iv = (int)v.v.d; break;
                        default:
                            iv = 0; break;
                    }
                    p->int_vals[i] = iv;
                }
                /* 当设置为 double 类型时，同时更新 double_vals */
                if(type_tag == 1 /* CAST_DOUBLE */ && p->double_vals) {
                    Value v = p->vals[i];
                    double dv = 0.0;
                    switch(v.type) {
                        case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                            dv = (double)v.v.i; break;
                        case 2:  // VAL_DOUBLE
                            dv = v.v.d; break;
                        default:
                            dv = 0.0; break;
                    }
                    p->double_vals[i] = dv;
                }
                /* 当设置为 float 类型时，同时更新 float_vals */
                if(type_tag == CAST_FLOAT && p->float_vals) {
                    Value v = p->vals[i];
                    float fv = 0.0f;
                    switch(v.type) {
                        case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                            fv = (float)v.v.i; break;
                        case 2:  // VAL_DOUBLE
                            fv = (float)v.v.d; break;
                        default:
                            fv = 0.0f; break;
                    }
                    p->float_vals[i] = fv;
                }
                /* 当设置为 uint 类型时，同时更新 uint32_vals */
                if(type_tag == CAST_UINT32 && p->uint32_vals) {
                    Value v = p->vals[i];
                    unsigned int uv = 0;
                    switch(v.type) {
                        case VAL_INT: case VAL_BYTE: case VAL_CHAR: case VAL_BOOL:
                            uv = (unsigned int)v.v.i; break;
                        case VAL_DOUBLE:
                            uv = (unsigned int)v.v.d; break;
                        default:
                            uv = 0; break;
                    }
                    p->uint32_vals[i] = uv;
                }
                /* 当设置为 bool 类型时，同时更新 bool_vals */
                if(type_tag == CAST_BOOL && p->bool_vals) {
                    Value v = p->vals[i];
                    _Bool bv = 0;
                    switch(v.type) {
                        case VAL_INT: case VAL_BYTE:
                            bv = v.v.i ? 1 : 0; break;
                        case VAL_CHAR:
                            bv = v.v.c ? 1 : 0; break;
                        case VAL_BOOL:
                            bv = v.v.b ? 1 : 0; break;
                        case VAL_DOUBLE:
                            bv = v.v.d ? 1 : 0; break;
                        default:
                            bv = 0; break;
                    }
                    p->bool_vals[i] = bv;
                }
                /* 当设置为 char 类型时，同时更新 char_vals */
                if(type_tag == CAST_CHAR && p->char_vals) {
                    Value v = p->vals[i];
                    char cv = 0;
                    switch(v.type) {
                        case VAL_INT: case VAL_BYTE:
                            cv = (char)v.v.i; break;
                        case VAL_CHAR:
                            cv = v.v.c; break;
                        case VAL_BOOL:
                            cv = v.v.b ? 1 : 0; break;
                        case VAL_DOUBLE:
                            cv = (char)v.v.d; break;
                        default:
                            cv = 0; break;
                    }
                    p->char_vals[i] = cv;
                }
                /* 当设置为 byte 类型时，同时更新 byte_vals */
                if(type_tag == CAST_BYTE && p->byte_vals) {
                    Value v = p->vals[i];
                    unsigned char bv = 0;
                    switch(v.type) {
                        case VAL_INT: case VAL_BYTE:
                            bv = (unsigned char)v.v.i; break;
                        case VAL_CHAR:
                            bv = (unsigned char)v.v.c; break;
                        case VAL_BOOL:
                            bv = v.v.b ? 1 : 0; break;
                        case VAL_DOUBLE:
                            bv = (unsigned char)v.v.d; break;
                        default:
                            bv = 0; break;
                    }
                    p->byte_vals[i] = bv;
                }
                /* 当设置为 int8 类型时，同时更新 int8_vals */
                if(type_tag == CAST_INT8 && p->int8_vals) {
                    Value v = p->vals[i];
                    int8_t i8v = 0;
                    switch(v.type) {
                        case VAL_INT: case VAL_INT8:
                            i8v = (int8_t)v.v.i; break;
                        case VAL_BYTE:
                            i8v = (int8_t)v.v.i; break;
                        case VAL_CHAR:
                            i8v = (int8_t)v.v.c; break;
                        case VAL_BOOL:
                            i8v = v.v.b ? 1 : 0; break;
                        case VAL_DOUBLE:
                            i8v = (int8_t)v.v.d; break;
                        default:
                            i8v = 0; break;
                    }
                    p->int8_vals[i] = i8v;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
}

/* 获取变量的类型标记（-1 表示无精确类型） */
int stackframe_get_type_tag(StackFrame* f, const char* name)
{
    if(!f || !name) return -1;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(hl) pthread_rwlock_unlock(&p->rw);
                return tag;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return -1;
}

/* 获取 int 类型变量的原始 int 值，零提取、零类型检查
   直接从 int_vals 数组读取，用于 OPC_LOAD_INT_VAR 指令
   如果变量不存在或不是 int 类型，返回 0 并置 *found=0 */
int stackframe_get_int(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                /* 检查变量是否标记为 int 类型 */
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 2 /* CAST_INT */ && p->int_vals) {
                    int iv = p->int_vals[i];  // 直接读取，零提取
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    if(found) *found = 1;
                    return iv;
                }
                /* 不是 int 类型，回退到从 Value 提取 */
                Value v = p->vals[i];
                int iv = 0;
                switch(v.type) {
                    case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                        iv = (int)v.v.i; break;
                    case 2:  // VAL_DOUBLE
                        iv = (int)v.v.d; break;
                    default:
                        iv = 0; break;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                if(found) *found = 1;
                return iv;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return 0;
}

void stackframe_set(StackFrame* f, const char* name, Value v)
{
    if(!f || !name) return;
    StackFrame* owner = NULL;
    int owner_cell = -1;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        int ci = find_cell_in_frame(p, name);
        if(ci >= 0) { owner = p; owner_cell = ci; }
        else if(find_in_frame(p, name) >= 0) { owner = p; owner_cell = -1; }
        if(hl) pthread_rwlock_unlock(&p->rw);
        if(owner) break;
    }
    if(owner) {
        int hl = owner->shared ? (pthread_rwlock_wrlock(&owner->rw), 1) : 0;
        if(owner_cell >= 0) {
            *(owner->cells[owner_cell]) = v;
        } else {
            int idx = find_in_frame(owner, name);   // 锁内重查
            if(idx >= 0) {
                slot_release(&owner->vals[idx]);
                owner->vals[idx] = v;
                /* 同步更新 int_vals：如果变量标记为 int 类型，同时更新原始 int 值 */
                int tag = (owner->type_tags) ? owner->type_tags[idx] : -1;
                if(tag == 2 /* CAST_INT */ && owner->int_vals) {
                    int iv = 0;
                    switch(v.type) {
                        case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                            iv = (int)v.v.i; break;
                        case 2:  // VAL_DOUBLE
                            iv = (int)v.v.d; break;
                        default:
                            iv = 0; break;
                    }
                    owner->int_vals[idx] = iv;
                }
            }
        }
        if(hl) pthread_rwlock_unlock(&owner->rw);
        return;
    }
    stackframe_bind(f, name, v);
}

void stackframe_bind(StackFrame* f, const char* name, Value v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int ci = find_cell_in_frame(f, name);
    if(ci >= 0) {
        /* 当前帧 cell：被捕获变量，写经堆单元（引用语义） */
        *(f->cells[ci]) = v;
        if(hl) pthread_rwlock_unlock(&f->rw);
        return;
    }
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx] = v;
        /* 同步更新 int_vals：如果变量标记为 int 类型，同时更新原始 int 值 */
        int tag = (f->type_tags) ? f->type_tags[idx] : -1;
        if(tag == 2 /* CAST_INT */ && f->int_vals) {
            int iv = 0;
            switch(v.type) {
                case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                    iv = (int)v.v.i; break;
                case 2:  // VAL_DOUBLE
                    iv = (int)v.v.d; break;
                default:
                    iv = 0; break;
            }
            f->int_vals[idx] = iv;
        }
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt] = v;
        /* 新建变量时 int_vals 初始化为 0（frame_ensure 已初始化）
           后续通过 stackframe_set_type_tag 设置类型标记时会更新 int_vals */
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

/* 绑定 int 变量：同时更新 vals（包装成 Value）和 int_vals（原始 int 值），零重复提取
   用于 OPC_STORE_INT_VAR 指令，避免从 Value 重复提取 int 值 */
void stackframe_bind_int(StackFrame* f, const char* name, int iv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        /* 变量已存在：更新 vals 和 int_vals */
        slot_release(&f->vals[idx]);
        f->vals[idx].type = 1;  // VAL_INT
        f->vals[idx].v.i = iv;
        if(f->int_vals) f->int_vals[idx] = iv;  // 直接更新 int_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 2;  // CAST_INT
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 1;  // VAL_INT
        f->vals[f->cnt].v.i = iv;
        if(f->int_vals) f->int_vals[f->cnt] = iv;  // 直接设置 int_vals，零提取！
        if(f->type_tags) f->type_tags[f->cnt] = 2;  // CAST_INT
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

/* 获取 double 变量：直接从 double_vals 数组读取，零提取、零类型检查
   直接从 double_vals 数组读取，用于 OPC_LOAD_DOUBLE_VAR 指令
   如果变量不存在或不是 double 类型，返回 0.0 并置 *found=0 */
double stackframe_get_double(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0.0;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                /* 检查变量是否标记为 double 类型（CAST_DOUBLE = 1） */
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 1 /* CAST_DOUBLE */ && p->double_vals) {
                    double dv = p->double_vals[i];  // 直接读取，零提取
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    if(found) *found = 1;
                    return dv;
                }
                /* 不是 double 类型，回退到从 Value 提取 */
                Value v = p->vals[i];
                double dv = 0.0;
                switch(v.type) {
                    case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                        dv = (double)v.v.i; break;
                    case 2:  // VAL_DOUBLE
                        dv = v.v.d; break;
                    default:
                        dv = 0.0; break;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                if(found) *found = 1;
                return dv;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return 0.0;
}

/* 绑定 double 变量：同时更新 vals（包装成 Value）和 double_vals（原始 double 值），零重复提取
   用于 OPC_STORE_DOUBLE_VAR 指令，避免从 Value 重复提取 double 值 */
void stackframe_bind_double(StackFrame* f, const char* name, double dv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        /* 变量已存在：更新 vals 和 double_vals */
        slot_release(&f->vals[idx]);
        f->vals[idx].type = 2;  // VAL_DOUBLE
        f->vals[idx].v.d = dv;
        if(f->double_vals) f->double_vals[idx] = dv;  // 直接更新 double_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 1;  // CAST_DOUBLE
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 2;  // VAL_DOUBLE
        f->vals[f->cnt].v.d = dv;
        if(f->double_vals) f->double_vals[f->cnt] = dv;  // 直接设置 double_vals，零提取！
        if(f->type_tags) f->type_tags[f->cnt] = 1;  // CAST_DOUBLE
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

/* 获取 float 变量：直接从 float_vals 数组读取，零提取、零类型检查
   直接从 float_vals 数组读取，用于 OPC_LOAD_FLOAT_VAR 指令
   如果变量不存在或不是 float 类型，返回 0.0f 并置 *found=0 */
float stackframe_get_float(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0.0f;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                /* 检查变量是否标记为 float 类型（CAST_FLOAT = 17） */
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == CAST_FLOAT && p->float_vals) {
                    float fv = p->float_vals[i];  // 直接读取，零提取
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    if(found) *found = 1;
                    return fv;
                }
                /* 不是 float 类型，回退到从 Value 提取 */
                Value v = p->vals[i];
                float fv = 0.0f;
                switch(v.type) {
                    case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                        fv = (float)v.v.i; break;
                    case 2:  // VAL_DOUBLE
                        fv = (float)v.v.d; break;
                    default:
                        fv = 0.0f; break;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                if(found) *found = 1;
                return fv;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return 0.0f;
}

/* 绑定 float 变量：同时更新 vals（包装成 Value）和 float_vals（原始 float 值），零重复提取
   用于 OPC_STORE_FLOAT_VAR 指令，避免从 Value 重复提取 float 值 */
void stackframe_bind_float(StackFrame* f, const char* name, float fv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        /* 变量已存在：更新 vals 和 float_vals */
        slot_release(&f->vals[idx]);
        f->vals[idx].type = 2;  // VAL_DOUBLE（float 用 VAL_DOUBLE 存储）
        f->vals[idx].v.d = (double)fv;
        if(f->float_vals) f->float_vals[idx] = fv;  // 直接更新 float_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 17;  // CAST_FLOAT
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 2;  // VAL_DOUBLE（float 用 VAL_DOUBLE 存储）
        f->vals[f->cnt].v.d = (double)fv;
        if(f->float_vals) f->float_vals[f->cnt] = fv;  // 直接设置 float_vals，零提取！
        if(f->type_tags) f->type_tags[f->cnt] = 17;  // CAST_FLOAT
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

/* 获取 uint 变量：直接从 uint32_vals 数组读取，零提取、零类型检查
   直接从 uint32_vals 数组读取，用于 OPC_LOAD_UINT_VAR 指令
   如果变量不存在或不是 uint 类型，返回 0 并置 *found=0 */
unsigned int stackframe_get_uint(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                /* 检查变量是否标记为 uint 类型（CAST_UINT32） */
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == CAST_UINT32 && p->uint32_vals) {
                    unsigned int uv = p->uint32_vals[i];  // 直接读取，零提取
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    if(found) *found = 1;
                    return uv;
                }
                /* 不是 uint 类型，回退到从 Value 提取 */
                Value v = p->vals[i];
                unsigned int uv = 0;
                switch(v.type) {
                    case VAL_INT: case VAL_BYTE: case VAL_CHAR: case VAL_BOOL:
                        uv = (unsigned int)v.v.i; break;
                    case VAL_DOUBLE:
                        uv = (unsigned int)v.v.d; break;
                    default:
                        uv = 0; break;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                if(found) *found = 1;
                return uv;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return 0;
}

/* 绑定 uint 变量：同时更新 vals（包装成 Value）和 uint32_vals（原始 uint 值），零重复提取
   用于 OPC_STORE_UINT_VAR 指令，避免从 Value 重复提取 uint 值 */
void stackframe_bind_uint(StackFrame* f, const char* name, unsigned int uv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        /* 变量已存在：更新 vals 和 uint32_vals */
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_INT;  // uint 用 VAL_INT 存储（long long 可以存储 uint32_t）
        f->vals[idx].v.i = (long long)uv;
        if(f->uint32_vals) f->uint32_vals[idx] = uv;  // 直接更新 uint32_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = CAST_UINT32;
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT;  // uint 用 VAL_INT 存储
        f->vals[f->cnt].v.i = (long long)uv;
        if(f->uint32_vals) f->uint32_vals[f->cnt] = uv;  // 直接设置 uint32_vals，零提取！
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT32;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

_Bool stackframe_get_bool(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            _Bool bv = fr->bool_vals ? fr->bool_vals[idx] : (fr->vals[idx].v.i ? 1 : 0);
            if(hl) pthread_rwlock_unlock(&f->rw);
            return bv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_bool(StackFrame* f, const char* name, _Bool bv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_BOOL;
        f->vals[idx].v.i = bv ? 1 : 0;
        if(f->bool_vals) f->bool_vals[idx] = bv;
        if(f->type_tags) f->type_tags[idx] = CAST_BOOL;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_BOOL;
        f->vals[f->cnt].v.i = bv ? 1 : 0;
        if(f->bool_vals) f->bool_vals[f->cnt] = bv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_BOOL;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

char stackframe_get_char(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            char cv = fr->char_vals ? fr->char_vals[idx] : (char)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return cv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_char(StackFrame* f, const char* name, char cv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_CHAR;
        f->vals[idx].v.i = (long long)cv;
        if(f->char_vals) f->char_vals[idx] = cv;
        if(f->type_tags) f->type_tags[idx] = CAST_CHAR;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_CHAR;
        f->vals[f->cnt].v.i = (long long)cv;
        if(f->char_vals) f->char_vals[f->cnt] = cv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_CHAR;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

unsigned char stackframe_get_byte(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            unsigned char bv = fr->byte_vals ? fr->byte_vals[idx] : (unsigned char)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return bv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_byte(StackFrame* f, const char* name, unsigned char bv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_BYTE;
        f->vals[idx].v.i = (long long)bv;
        if(f->byte_vals) f->byte_vals[idx] = bv;
        if(f->type_tags) f->type_tags[idx] = CAST_BYTE;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_BYTE;
        f->vals[f->cnt].v.i = (long long)bv;
        if(f->byte_vals) f->byte_vals[f->cnt] = bv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_BYTE;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

int8_t stackframe_get_int8(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            int8_t i8v = fr->int8_vals ? fr->int8_vals[idx] : (int8_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return i8v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_int8(StackFrame* f, const char* name, int8_t i8v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_INT8;
        f->vals[idx].v.i = (long long)i8v;
        if(f->int8_vals) f->int8_vals[idx] = i8v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT8;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT8;
        f->vals[f->cnt].v.i = (long long)i8v;
        if(f->int8_vals) f->int8_vals[f->cnt] = i8v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT8;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

int16_t stackframe_get_int16(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            int16_t i16v = fr->int16_vals ? fr->int16_vals[idx] : (int16_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return i16v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_int16(StackFrame* f, const char* name, int16_t i16v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_INT16;
        f->vals[idx].v.i = (long long)i16v;
        if(f->int16_vals) f->int16_vals[idx] = i16v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT16;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT16;
        f->vals[f->cnt].v.i = (long long)i16v;
        if(f->int16_vals) f->int16_vals[f->cnt] = i16v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT16;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

int32_t stackframe_get_int32(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            int32_t i32v = fr->int32_vals ? fr->int32_vals[idx] : (int32_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return i32v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_int32(StackFrame* f, const char* name, int32_t i32v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_INT32;
        f->vals[idx].v.i = (long long)i32v;
        if(f->int32_vals) f->int32_vals[idx] = i32v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT32;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT32;
        f->vals[f->cnt].v.i = (long long)i32v;
        if(f->int32_vals) f->int32_vals[f->cnt] = i32v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT32;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

int64_t stackframe_get_int64(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            int64_t i64v = fr->int64_vals ? fr->int64_vals[idx] : (int64_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return i64v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_int64(StackFrame* f, const char* name, int64_t i64v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_INT64;
        f->vals[idx].v.i = (long long)i64v;
        if(f->int64_vals) f->int64_vals[idx] = i64v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT64;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT64;
        f->vals[f->cnt].v.i = (long long)i64v;
        if(f->int64_vals) f->int64_vals[f->cnt] = i64v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT64;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

uint8_t stackframe_get_uint8(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            uint8_t u8v = fr->uint8_vals ? fr->uint8_vals[idx] : (uint8_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return u8v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_uint8(StackFrame* f, const char* name, uint8_t u8v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_UINT8;
        f->vals[idx].v.i = (long long)u8v;
        if(f->uint8_vals) f->uint8_vals[idx] = u8v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT8;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT8;
        f->vals[f->cnt].v.i = (long long)u8v;
        if(f->uint8_vals) f->uint8_vals[f->cnt] = u8v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT8;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

uint16_t stackframe_get_uint16(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            uint16_t u16v = fr->uint16_vals ? fr->uint16_vals[idx] : (uint16_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return u16v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_uint16(StackFrame* f, const char* name, uint16_t u16v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_UINT16;
        f->vals[idx].v.i = (long long)u16v;
        if(f->uint16_vals) f->uint16_vals[idx] = u16v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT16;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT16;
        f->vals[f->cnt].v.i = (long long)u16v;
        if(f->uint16_vals) f->uint16_vals[f->cnt] = u16v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT16;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

uint64_t stackframe_get_uint64(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            uint64_t u64v = fr->uint64_vals ? fr->uint64_vals[idx] : (uint64_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return u64v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_uint64(StackFrame* f, const char* name, uint64_t u64v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_UINT64;
        f->vals[idx].v.i = (long long)u64v;
        if(f->uint64_vals) f->uint64_vals[idx] = u64v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT64;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT64;
        f->vals[f->cnt].v.i = (long long)u64v;
        if(f->uint64_vals) f->uint64_vals[f->cnt] = u64v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT64;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

long stackframe_get_long(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            long lv = fr->long_vals ? fr->long_vals[idx] : (long)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return lv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_long(StackFrame* f, const char* name, long lv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_LONG;
        f->vals[idx].v.i = (long long)lv;
        if(f->long_vals) f->long_vals[idx] = lv;
        if(f->type_tags) f->type_tags[idx] = CAST_LONG;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_LONG;
        f->vals[f->cnt].v.i = (long long)lv;
        if(f->long_vals) f->long_vals[f->cnt] = lv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_LONG;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

unsigned long stackframe_get_ulong(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            unsigned long ulv = fr->ulong_vals ? fr->ulong_vals[idx] : (unsigned long)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return ulv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_ulong(StackFrame* f, const char* name, unsigned long ulv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_ULONG;
        f->vals[idx].v.i = (long long)ulv;
        if(f->ulong_vals) f->ulong_vals[idx] = ulv;
        if(f->type_tags) f->type_tags[idx] = CAST_ULONG;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_ULONG;
        f->vals[f->cnt].v.i = (long long)ulv;
        if(f->ulong_vals) f->ulong_vals[f->cnt] = ulv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_ULONG;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

size_t stackframe_get_size_t(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            size_t stv = fr->size_t_vals ? fr->size_t_vals[idx] : (size_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return stv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_size_t(StackFrame* f, const char* name, size_t stv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_SIZE_T;
        f->vals[idx].v.i = (long long)stv;
        if(f->size_t_vals) f->size_t_vals[idx] = stv;
        if(f->type_tags) f->type_tags[idx] = CAST_SIZE_T;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_SIZE_T;
        f->vals[f->cnt].v.i = (long long)stv;
        if(f->size_t_vals) f->size_t_vals[f->cnt] = stv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_SIZE_T;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

// cell 表扩容（调用方须已持锁）
static void cell_ensure(StackFrame* f, int need)
{
    if(need <= f->cell_cap) return;
    int nc = f->cell_cap > 0 ? f->cell_cap * 2 : 8;
    while(nc < need) nc *= 2;
    char** nn = (char**)malloc((size_t)nc * sizeof(char*));
    Value** nv = (Value**)malloc((size_t)nc * sizeof(Value*));
    if(!nn || !nv) { perror("stackframe cell expand"); exit(EXIT_FAILURE); }
    if(f->cell_names) memcpy(nn, f->cell_names, (size_t)f->cell_cap * sizeof(char*));
    if(f->cells) memcpy(nv, f->cells, (size_t)f->cell_cap * sizeof(Value*));
    for(int i = f->cell_cap; i < nc; i++) { nn[i] = NULL; nv[i] = NULL; }
    free(f->cell_names); free(f->cells);
    f->cell_names = nn; f->cells = nv;
    f->cell_cap = nc;
}

void stackframe_add_cell(StackFrame* f, const char* name, Value* cell_ptr)
{
    if(!f || !name || !cell_ptr) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int ci = find_cell_in_frame(f, name);
    if(ci >= 0) {
        f->cells[ci] = cell_ptr;
    } else {
        cell_ensure(f, f->cell_cnt + 1);
        f->cell_names[f->cell_cnt] = strdup(name);
        f->cells[f->cell_cnt] = cell_ptr;
        f->cell_cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

Value** stackframe_find_cell(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        int ci = find_cell_in_frame(p, name);
        if(hl) pthread_rwlock_unlock(&p->rw);
        if(ci >= 0) return &p->cells[ci];
    }
    return NULL;
}

Value* stackframe_ensure_cell(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    /* 已存在 cell：直接返回 */
    Value** exist = stackframe_find_cell(f, name);
    if(exist) return *exist;
    /* 沿链定位变量所在帧（普通槽位），在该帧内装箱 */
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_wrlock(&p->rw), 1) : 0;
        int idx = find_in_frame(p, name);
        if(idx >= 0) {
            /* 分配堆 cell，拷贝当前值；cell 生命周期由闭包持有 */
            Value* cell = (Value*)malloc(sizeof(Value));
            if(!cell) { perror("closure cell alloc"); exit(EXIT_FAILURE); }
            *cell = p->vals[idx];
            cell_ensure(p, p->cell_cnt + 1);
            p->cell_names[p->cell_cnt] = strdup(name);
            p->cells[p->cell_cnt] = cell;
            p->cell_cnt++;
            if(hl) pthread_rwlock_unlock(&p->rw);
            return cell;
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}
