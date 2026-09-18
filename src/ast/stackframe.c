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

/* 线程本地帧空闲链表：stackframe_new/destroy 不再 malloc/free，
 * 而是从 TLS 空闲链表取出/放回。帧内数组（names/vals/int_slots 等）也保留复用，
 * 高并发下每次函数调用省 6+ 次 malloc。 */
static _Thread_local StackFrame* g_frame_freelist = NULL;

StackFrame* stackframe_new(StackFrame* parent)
{
    StackFrame* f;
    if(g_frame_freelist) {
        /* 从空闲链表取出，清零使用状态但保留已分配数组 */
        f = g_frame_freelist;
        g_frame_freelist = *(StackFrame**)f;  /* next 指针存在 parent 字段 */
        f->cnt = 0;
        f->parent = parent;
        f->shared = 0;
        f->cell_cnt = 0;
        f->cell_cap = 0;
        /* names/vals/int_slots/flt_slots/ptr_slots/type_tags 保留，cap 保留，直接复用 */
    } else {
        f = (StackFrame*)calloc(1, sizeof(StackFrame));
        if(!f) { perror("stackframe_new"); exit(EXIT_FAILURE); }
        f->cnt = 0;
        f->cap = 0;
        f->names = NULL;
        f->vals = NULL;
        f->int_slots = NULL;
        f->flt_slots = NULL;
        f->ptr_slots = NULL;
        f->type_tags = NULL;
        f->parent = parent;
        f->shared = 0;
        f->cell_names = NULL;
        f->cells = NULL;
        f->cell_cnt = 0;
        f->cell_cap = 0;
    }
    /* 私有帧不初始化读写锁（零开销）；共享帧在 set_shared 时才初始化 */
    return f;
}

void stackframe_set_shared(StackFrame* f)
{
    if(f) { f->shared = 1; pthread_rwlock_init(&f->rw, NULL); }
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

    /* 扩容 int_slots（所有整数/布尔/字符类型统一存为 int64_t） */
    int64_t* nint_slots = (int64_t*)malloc((size_t)newcap * sizeof(int64_t));
    if(!nint_slots) { perror("stackframe expand int_slots"); exit(EXIT_FAILURE); }
    if(f->int_slots) memcpy(nint_slots, f->int_slots, (size_t)f->cap * sizeof(int64_t));
    for(int i = f->cap; i < newcap; i++) nint_slots[i] = (int64_t)0;
    int64_t* old_int_slots = f->int_slots;
    f->int_slots = nint_slots;
    free(old_int_slots);

    /* 扩容 flt_slots（所有浮点类型统一存为 double） */
    double* nflt_slots = (double*)malloc((size_t)newcap * sizeof(double));
    if(!nflt_slots) { perror("stackframe expand flt_slots"); exit(EXIT_FAILURE); }
    if(f->flt_slots) memcpy(nflt_slots, f->flt_slots, (size_t)f->cap * sizeof(double));
    for(int i = f->cap; i < newcap; i++) nflt_slots[i] = (double)0.0;
    double* old_flt_slots = f->flt_slots;
    f->flt_slots = nflt_slots;
    free(old_flt_slots);

    /* 扩容 ptr_slots（所有指针类型统一存为 void*） */
    void** nptr_slots = (void**)malloc((size_t)newcap * sizeof(void*));
    if(!nptr_slots) { perror("stackframe expand ptr_slots"); exit(EXIT_FAILURE); }
    if(f->ptr_slots) memcpy(nptr_slots, f->ptr_slots, (size_t)f->cap * sizeof(void*));
    for(int i = f->cap; i < newcap; i++) nptr_slots[i] = NULL;
    void** old_ptr_slots = f->ptr_slots;
    f->ptr_slots = nptr_slots;
    free(old_ptr_slots);

    f->cap = newcap;
}

void stackframe_destroy(StackFrame* f)
{
    if(!f) return;

    /* 共享帧：完整释放（其他线程可能仍在访问） */
    if(f->shared) {
        for(int i = 0; i < f->cnt; i++) {
            free(f->names[i]);
            slot_release(&f->vals[i]);
        }
        free(f->names);
        free(f->vals);
        free(f->int_slots);
        free(f->flt_slots);
        free(f->ptr_slots);
        for(int i = 0; i < f->cell_cnt; i++) free(f->cell_names[i]);
        free(f->cell_names);
        free(f->cells);
        free(f->type_tags);
        pthread_rwlock_destroy(&f->rw);
        free(f);
        return;
    }

    /* 私有帧：释放名称字符串和 Value 内容，但保留数组本身复用 */
    for(int i = 0; i < f->cnt; i++) {
        free(f->names[i]);
        slot_release(&f->vals[i]);
    }
    /* 释放 cell 表（闭包 cell 指针由 RuntimeFunc 持有，不释放） */
    for(int i = 0; i < f->cell_cnt; i++) free(f->cell_names[i]);
    /* 清空名称指针（复用帧时 names 数组保留，名称重新分配） */
    for(int i = 0; i < f->cnt; i++) f->names[i] = NULL;

    /* 放回线程本地空闲链表（next 指针存在 parent 字段位置） */
    *(StackFrame**)f = g_frame_freelist;
    g_frame_freelist = f;
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
                if(type_tag == 2 /* CAST_INT */ && p->int_slots) {
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
                    p->int_slots[i] = iv;
                }
                /* 当设置为 double 类型时，同时更新 double_vals */
                if(type_tag == 1 /* CAST_DOUBLE */ && p->flt_slots) {
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
                    p->flt_slots[i] = dv;
                }
                /* 当设置为 float 类型时，同时更新 float_vals */
                if(type_tag == CAST_FLOAT && p->flt_slots) {
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
                    p->flt_slots[i] = fv;
                }
                /* 当设置为 uint 类型时，同时更新 uint32_vals */
                if(type_tag == CAST_UINT32 && p->int_slots) {
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
                    p->int_slots[i] = uv;
                }
                /* 当设置为 bool 类型时，同时更新 bool_vals */
                if(type_tag == CAST_BOOL && p->int_slots) {
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
                    p->int_slots[i] = bv;
                }
                /* 当设置为 char 类型时，同时更新 char_vals */
                if(type_tag == CAST_CHAR && p->int_slots) {
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
                    p->int_slots[i] = cv;
                }
                /* 当设置为 byte 类型时，同时更新 byte_vals */
                if(type_tag == CAST_BYTE && p->int_slots) {
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
                    p->int_slots[i] = bv;
                }
                /* 当设置为 int8 类型时，同时更新 int8_vals */
                if(type_tag == CAST_INT8 && p->int_slots) {
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
                    p->int_slots[i] = i8v;
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
                if(tag == 2 /* CAST_INT */ && p->int_slots) {
                    int iv = p->int_slots[i];  // 直接读取，零提取
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

// 获取 int 类型变量的原始指针，用于自增自减等直接操作
// 返回 NULL 表示未找到或不是 int 类型
int64_t* stackframe_get_int_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 2 /* CAST_INT */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];  // 返回原始指针
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;  // 找到了但不是 int 类型
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 int8 类型变量的原始指针
int64_t* stackframe_get_int8_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 16 /* CAST_INT8 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 int16 类型变量的原始指针
int64_t* stackframe_get_int16_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 17 /* CAST_INT16 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 short 类型变量的原始指针
int64_t* stackframe_get_short_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 65 /* CAST_SHORT */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 int32 类型变量的原始指针
int64_t* stackframe_get_int32_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 18 /* CAST_INT32 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 int64 类型变量的原始指针
int64_t* stackframe_get_int64_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 19 /* CAST_INT64 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 uint 类型变量的原始指针（uint 对应 uint32）
int64_t* stackframe_get_uint_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 23 /* CAST_UINT */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 uint8 类型变量的原始指针
int64_t* stackframe_get_uint8_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 20 /* CAST_UINT8 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 uint16 类型变量的原始指针
int64_t* stackframe_get_uint16_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 21 /* CAST_UINT16 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 uint32 类型变量的原始指针
int64_t* stackframe_get_uint32_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 22 /* CAST_UINT32 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
}

// 获取 uint64 类型变量的原始指针
int64_t* stackframe_get_uint64_ptr(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(StackFrame* p = f; p; p = p->parent) {
        int hl = p->shared ? (pthread_rwlock_rdlock(&p->rw), 1) : 0;
        for(int i = 0; i < p->cnt; i++) {
            if(strcmp(p->names[i], name) == 0) {
                int tag = (p->type_tags) ? p->type_tags[i] : -1;
                if(tag == 24 /* CAST_UINT64 */ && p->int_slots) {
                    int64_t* ptr = &p->int_slots[i];
                    if(hl) pthread_rwlock_unlock(&p->rw);
                    return ptr;
                }
                if(hl) pthread_rwlock_unlock(&p->rw);
                return NULL;
            }
        }
        if(hl) pthread_rwlock_unlock(&p->rw);
    }
    return NULL;
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
                if(tag == 2 /* CAST_INT */ && owner->int_slots) {
                    int iv = 0;
                    switch(v.type) {
                        case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                            iv = (int)v.v.i; break;
                        case 2:  // VAL_DOUBLE
                            iv = (int)v.v.d; break;
                        default:
                            iv = 0; break;
                    }
                    owner->int_slots[idx] = iv;
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
        if(tag == 2 /* CAST_INT */ && f->int_slots) {
            int iv = 0;
            switch(v.type) {
                case 1: case 10: case 4: case 3:  // VAL_INT, VAL_BYTE, VAL_CHAR, VAL_BOOL
                    iv = (int)v.v.i; break;
                case 2:  // VAL_DOUBLE
                    iv = (int)v.v.d; break;
                default:
                    iv = 0; break;
            }
            f->int_slots[idx] = iv;
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
        if(f->int_slots) f->int_slots[idx] = iv;  // 直接更新 int_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 2;  // CAST_INT
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 1;  // VAL_INT
        f->vals[f->cnt].v.i = iv;
        if(f->int_slots) f->int_slots[f->cnt] = iv;  // 直接设置 int_vals，零提取！
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
                if(tag == 1 /* CAST_DOUBLE */ && p->flt_slots) {
                    double dv = p->flt_slots[i];  // 直接读取，零提取
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
        if(f->flt_slots) f->flt_slots[idx] = dv;  // 直接更新 double_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 1;  // CAST_DOUBLE
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 2;  // VAL_DOUBLE
        f->vals[f->cnt].v.d = dv;
        if(f->flt_slots) f->flt_slots[f->cnt] = dv;  // 直接设置 double_vals，零提取！
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
                if(tag == CAST_FLOAT && p->flt_slots) {
                    float fv = p->flt_slots[i];  // 直接读取，零提取
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
        if(f->flt_slots) f->flt_slots[idx] = fv;  // 直接更新 float_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = 17;  // CAST_FLOAT
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = 2;  // VAL_DOUBLE（float 用 VAL_DOUBLE 存储）
        f->vals[f->cnt].v.d = (double)fv;
        if(f->flt_slots) f->flt_slots[f->cnt] = fv;  // 直接设置 float_vals，零提取！
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
                if(tag == CAST_UINT32 && p->int_slots) {
                    unsigned int uv = p->int_slots[i];  // 直接读取，零提取
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
        if(f->int_slots) f->int_slots[idx] = uv;  // 直接更新 uint32_vals，零提取！
        if(f->type_tags) f->type_tags[idx] = CAST_UINT32;
    } else {
        /* 变量不存在：新建 */
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT;  // uint 用 VAL_INT 存储
        f->vals[f->cnt].v.i = (long long)uv;
        if(f->int_slots) f->int_slots[f->cnt] = uv;  // 直接设置 uint32_vals，零提取！
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
            _Bool bv = fr->int_slots ? fr->int_slots[idx] : (fr->vals[idx].v.i ? 1 : 0);
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
        if(f->int_slots) f->int_slots[idx] = bv;
        if(f->type_tags) f->type_tags[idx] = CAST_BOOL;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_BOOL;
        f->vals[f->cnt].v.i = bv ? 1 : 0;
        if(f->int_slots) f->int_slots[f->cnt] = bv;
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
            char cv = fr->int_slots ? fr->int_slots[idx] : (char)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = cv;
        if(f->type_tags) f->type_tags[idx] = CAST_CHAR;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_CHAR;
        f->vals[f->cnt].v.i = (long long)cv;
        if(f->int_slots) f->int_slots[f->cnt] = cv;
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
            unsigned char bv = fr->int_slots ? fr->int_slots[idx] : (unsigned char)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = bv;
        if(f->type_tags) f->type_tags[idx] = CAST_BYTE;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_BYTE;
        f->vals[f->cnt].v.i = (long long)bv;
        if(f->int_slots) f->int_slots[f->cnt] = bv;
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
            int8_t i8v = fr->int_slots ? fr->int_slots[idx] : (int8_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = i8v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT8;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT8;
        f->vals[f->cnt].v.i = (long long)i8v;
        if(f->int_slots) f->int_slots[f->cnt] = i8v;
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
            int16_t i16v = fr->int_slots ? fr->int_slots[idx] : (int16_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return i16v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

short stackframe_get_short(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            short sv = fr->int_slots ? fr->int_slots[idx] : fr->vals[idx].v.sh;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return sv;
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
        if(f->int_slots) f->int_slots[idx] = i16v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT16;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT16;
        f->vals[f->cnt].v.i = (long long)i16v;
        if(f->int_slots) f->int_slots[f->cnt] = i16v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT16;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

void stackframe_bind_short(StackFrame* f, const char* name, short sv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_SHORT;
        f->vals[idx].v.sh = sv;
        if(f->int_slots) f->int_slots[idx] = sv;
        if(f->type_tags) f->type_tags[idx] = CAST_SHORT;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_SHORT;
        f->vals[f->cnt].v.sh = sv;
        if(f->int_slots) f->int_slots[f->cnt] = sv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_SHORT;
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
            int32_t i32v = fr->int_slots ? fr->int_slots[idx] : (int32_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = i32v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT32;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT32;
        f->vals[f->cnt].v.i = (long long)i32v;
        if(f->int_slots) f->int_slots[f->cnt] = i32v;
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
            int64_t i64v = fr->int_slots ? fr->int_slots[idx] : (int64_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = i64v;
        if(f->type_tags) f->type_tags[idx] = CAST_INT64;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_INT64;
        f->vals[f->cnt].v.i = (long long)i64v;
        if(f->int_slots) f->int_slots[f->cnt] = i64v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_INT64;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

long long stackframe_get_long_long(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            long long llv = fr->int_slots ? fr->int_slots[idx] : fr->vals[idx].v.ll;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return llv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_long_long(StackFrame* f, const char* name, long long llv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_LONG_LONG;
        f->vals[idx].v.ll = llv;
        if(f->int_slots) f->int_slots[idx] = llv;
        if(f->type_tags) f->type_tags[idx] = CAST_LONGLONG;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_LONG_LONG;
        f->vals[f->cnt].v.ll = llv;
        if(f->int_slots) f->int_slots[f->cnt] = llv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_LONGLONG;
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
            uint8_t u8v = fr->int_slots ? fr->int_slots[idx] : (uint8_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = u8v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT8;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT8;
        f->vals[f->cnt].v.i = (long long)u8v;
        if(f->int_slots) f->int_slots[f->cnt] = u8v;
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
            uint16_t u16v = fr->int_slots ? fr->int_slots[idx] : (uint16_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = u16v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT16;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT16;
        f->vals[f->cnt].v.i = (long long)u16v;
        if(f->int_slots) f->int_slots[f->cnt] = u16v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT16;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

uint32_t stackframe_get_uint32(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            uint32_t u32v = fr->int_slots ? fr->int_slots[idx] : (uint32_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return u32v;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_uint32(StackFrame* f, const char* name, uint32_t u32v)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_UINT32;
        f->vals[idx].v.i = (long long)u32v;
        if(f->int_slots) f->int_slots[idx] = u32v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT32;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT32;
        f->vals[f->cnt].v.i = (long long)u32v;
        if(f->int_slots) f->int_slots[f->cnt] = u32v;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UINT32;
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
            uint64_t u64v = fr->int_slots ? fr->int_slots[idx] : (uint64_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = u64v;
        if(f->type_tags) f->type_tags[idx] = CAST_UINT64;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UINT64;
        f->vals[f->cnt].v.i = (long long)u64v;
        if(f->int_slots) f->int_slots[f->cnt] = u64v;
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
            long lv = fr->int_slots ? fr->int_slots[idx] : (long)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = lv;
        if(f->type_tags) f->type_tags[idx] = CAST_LONG;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_LONG;
        f->vals[f->cnt].v.i = (long long)lv;
        if(f->int_slots) f->int_slots[f->cnt] = lv;
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
            unsigned long ulv = fr->int_slots ? fr->int_slots[idx] : (unsigned long)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = ulv;
        if(f->type_tags) f->type_tags[idx] = CAST_ULONG;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_ULONG;
        f->vals[f->cnt].v.i = (long long)ulv;
        if(f->int_slots) f->int_slots[f->cnt] = ulv;
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
            size_t stv = fr->int_slots ? fr->int_slots[idx] : (size_t)fr->vals[idx].v.i;
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
        if(f->int_slots) f->int_slots[idx] = stv;
        if(f->type_tags) f->type_tags[idx] = CAST_SIZE_T;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_SIZE_T;
        f->vals[f->cnt].v.i = (long long)stv;
        if(f->int_slots) f->int_slots[f->cnt] = stv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_SIZE_T;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

ssize_t stackframe_get_ssize_t(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            ssize_t sstv = fr->int_slots ? fr->int_slots[idx] : (ssize_t)fr->vals[idx].v.i;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return sstv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_ssize_t(StackFrame* f, const char* name, ssize_t sstv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_SSIZE_T;
        f->vals[idx].v.i = (long long)sstv;
        if(f->int_slots) f->int_slots[idx] = sstv;
        if(f->type_tags) f->type_tags[idx] = CAST_SSIZE_T;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_SSIZE_T;
        f->vals[f->cnt].v.i = (long long)sstv;
        if(f->int_slots) f->int_slots[f->cnt] = sstv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_SSIZE_T;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

long double stackframe_get_long_double(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            long double ldv = fr->flt_slots ? fr->flt_slots[idx] : (long double)fr->vals[idx].v.d;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return ldv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_long_double(StackFrame* f, const char* name, long double ldv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_LONG_DOUBLE;
        f->vals[idx].v.d = (double)ldv;
        if(f->flt_slots) f->flt_slots[idx] = ldv;
        if(f->type_tags) f->type_tags[idx] = CAST_LONG_DOUBLE;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_LONG_DOUBLE;
        f->vals[f->cnt].v.d = (double)ldv;
        if(f->flt_slots) f->flt_slots[f->cnt] = ldv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_LONG_DOUBLE;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

unsigned char stackframe_get_uchar(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            unsigned char ucv = fr->int_slots ? fr->int_slots[idx] : fr->vals[idx].v.uc;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return ucv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_uchar(StackFrame* f, const char* name, unsigned char ucv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_UCHAR;
        f->vals[idx].v.uc = ucv;
        if(f->int_slots) f->int_slots[idx] = ucv;
        if(f->type_tags) f->type_tags[idx] = CAST_UCHAR;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_UCHAR;
        f->vals[f->cnt].v.uc = ucv;
        if(f->int_slots) f->int_slots[f->cnt] = ucv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_UCHAR;
        f->cnt++;
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
}

unsigned short stackframe_get_ushort(StackFrame* f, const char* name, _Bool* found)
{
    if(!f || !name) { if(found) *found = 0; return 0; }
    int hl = f->shared ? (pthread_rwlock_rdlock(&f->rw), 1) : 0;
    for(StackFrame* fr = f; fr; fr = fr->parent) {
        int idx = find_in_frame(fr, name);
        if(idx >= 0) {
            if(found) *found = 1;
            unsigned short usv = fr->int_slots ? fr->int_slots[idx] : fr->vals[idx].v.us;
            if(hl) pthread_rwlock_unlock(&f->rw);
            return usv;
        }
    }
    if(hl) pthread_rwlock_unlock(&f->rw);
    if(found) *found = 0;
    return 0;
}

void stackframe_bind_ushort(StackFrame* f, const char* name, unsigned short usv)
{
    if(!f || !name) return;
    int hl = f->shared ? (pthread_rwlock_wrlock(&f->rw), 1) : 0;
    int idx = find_in_frame(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx].type = VAL_USHORT;
        f->vals[idx].v.us = usv;
        if(f->int_slots) f->int_slots[idx] = usv;
        if(f->type_tags) f->type_tags[idx] = CAST_USHORT;
    } else {
        frame_ensure(f, f->cnt + 1);
        f->names[f->cnt] = strdup(name);
        f->vals[f->cnt].type = VAL_USHORT;
        f->vals[f->cnt].v.us = usv;
        if(f->int_slots) f->int_slots[f->cnt] = usv;
        if(f->type_tags) f->type_tags[f->cnt] = CAST_USHORT;
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
