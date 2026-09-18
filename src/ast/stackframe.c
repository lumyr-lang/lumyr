#include "stackframe.h"
#include "lumyr_value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// 释放一个帧内槽位的资源
static void slot_release(Value* v) {
    if(!v) return;
    if(v->type == VAL_FUNC) {
        v->type = VAL_NONE;
        return;
    }
    val_destroy(v);
}

/* 线程本地帧空闲链表：高并发下减少 malloc/free */
static _Thread_local StackFrame* g_frame_freelist = NULL;

StackFrame* stackframe_new(StackFrame* parent)
{
    StackFrame* f;
    if(g_frame_freelist) {
        f = g_frame_freelist;
        g_frame_freelist = *(StackFrame**)f;
        f->cnt = 0;
        f->parent = parent;
        f->shared = 0;
        f->cell_cnt = 0;
    } else {
        f = (StackFrame*)calloc(1, sizeof(StackFrame));
        if(!f) { perror("stackframe_new"); exit(EXIT_FAILURE); }
        f->cnt = 0;
        f->cap = 0;
        f->parent = parent;
        f->shared = 0;
    }
    return f;
}

void stackframe_set_shared(StackFrame* f)
{
    if(f) { f->shared = 1; pthread_rwlock_init(&f->rw, NULL); }
}

/* 帧内变量槽扩容：翻倍，无硬上限
 * 使用 malloc + memcpy 而非 realloc，避免 GC UAF */
static void frame_ensure(StackFrame* f, int need)
{
    if(need <= f->cap) return;
    int newcap = f->cap > 0 ? f->cap : 16;
    while(newcap < need) newcap *= 2;

    /* 扩容 names */
    char** nn = (char**)malloc((size_t)newcap * sizeof(char*));
    if(!nn) { perror("stackframe expand names"); exit(EXIT_FAILURE); }
    if(f->names) memcpy(nn, f->names, (size_t)f->cap * sizeof(char*));
    for(int i = f->cap; i < newcap; i++) nn[i] = NULL;
    char** old_names = f->names;
    f->names = nn;
    free(old_names);

    /* 扩容 vals */
    Value* nv = (Value*)malloc((size_t)newcap * sizeof(Value));
    if(!nv) { perror("stackframe expand vals"); exit(EXIT_FAILURE); }
    if(f->vals) memcpy(nv, f->vals, (size_t)f->cap * sizeof(Value));
    for(int i = f->cap; i < newcap; i++) {
        nv[i].type = VAL_NONE;
        nv[i].v.i = 0;
    }
    Value* old_vals = f->vals;
    f->vals = nv;
    free(old_vals);

    /* 扩容 int_slots（所有整数/布尔/字符统一存为 int64_t） */
    int64_t* nint = (int64_t*)malloc((size_t)newcap * sizeof(int64_t));
    if(!nint) { perror("stackframe expand int_slots"); exit(EXIT_FAILURE); }
    if(f->int_slots) memcpy(nint, f->int_slots, (size_t)f->cap * sizeof(int64_t));
    for(int i = f->cap; i < newcap; i++) nint[i] = 0;
    int64_t* old_int = f->int_slots;
    f->int_slots = nint;
    free(old_int);

    /* 扩容 flt_slots（所有浮点统一存为 double） */
    double* nflt = (double*)malloc((size_t)newcap * sizeof(double));
    if(!nflt) { perror("stackframe expand flt_slots"); exit(EXIT_FAILURE); }
    if(f->flt_slots) memcpy(nflt, f->flt_slots, (size_t)f->cap * sizeof(double));
    for(int i = f->cap; i < newcap; i++) nflt[i] = 0.0;
    double* old_flt = f->flt_slots;
    f->flt_slots = nflt;
    free(old_flt);

    /* 扩容 ptr_slots（所有指针/字符串统一存为 void*） */
    void** nptr = (void**)malloc((size_t)newcap * sizeof(void*));
    if(!nptr) { perror("stackframe expand ptr_slots"); exit(EXIT_FAILURE); }
    if(f->ptr_slots) memcpy(nptr, f->ptr_slots, (size_t)f->cap * sizeof(void*));
    for(int i = f->cap; i < newcap; i++) nptr[i] = NULL;
    void** old_ptr = f->ptr_slots;
    f->ptr_slots = nptr;
    free(old_ptr);

    /* 扩容 type_tags */
    int* ntag = (int*)malloc((size_t)newcap * sizeof(int));
    if(!ntag) { perror("stackframe expand type_tags"); exit(EXIT_FAILURE); }
    if(f->type_tags) memcpy(ntag, f->type_tags, (size_t)f->cap * sizeof(int));
    for(int i = f->cap; i < newcap; i++) ntag[i] = -1;
    int* old_tag = f->type_tags;
    f->type_tags = ntag;
    free(old_tag);

    f->cap = newcap;
}

void stackframe_destroy(StackFrame* f)
{
    if(!f) return;

    /* 释放槽位资源 */
    for(int i = 0; i < f->cnt; i++) {
        if(f->names[i]) free(f->names[i]);
        slot_release(&f->vals[i]);
    }

    free(f->names);
    free(f->vals);
    free(f->int_slots);
    free(f->flt_slots);
    free(f->ptr_slots);
    free(f->type_tags);
    free(f->cell_names);
    free(f->cells);

    if(f->shared) {
        pthread_rwlock_destroy(&f->rw);
    }

    /* 放回空闲链表 */
    *(StackFrame**)f = g_frame_freelist;
    g_frame_freelist = f;
}

/* 沿 parent 链查找变量索引，返回当前帧的槽位索引；找不到返回 -1 */
static int frame_find(StackFrame* f, const char* name)
{
    if(!f || !name) return -1;
    for(int i = 0; i < f->cnt; i++) {
        if(f->names[i] && strcmp(f->names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

Value stackframe_get(StackFrame* f, const char* name, _Bool* found)
{
    Value zero = {0};
    zero.type = VAL_NONE;
    if(found) *found = 0;
    if(!f || !name) return zero;

    StackFrame* cur = f;
    while(cur) {
        int idx = frame_find(cur, name);
        if(idx >= 0) {
            if(found) *found = 1;
            return cur->vals[idx];
        }
        cur = cur->parent;
    }
    return zero;
}

void stackframe_set(StackFrame* f, const char* name, Value v)
{
    if(!f || !name) return;

    StackFrame* cur = f;
    while(cur) {
        int idx = frame_find(cur, name);
        if(idx >= 0) {
            slot_release(&cur->vals[idx]);
            cur->vals[idx] = v;
            return;
        }
        cur = cur->parent;
    }

    /* 找不到，在当前帧新建 */
    frame_ensure(f, f->cnt + 1);
    int idx = f->cnt++;
    f->names[idx] = strdup(name);
    f->vals[idx] = v;
    f->int_slots[idx] = 0;
    f->flt_slots[idx] = 0.0;
    f->ptr_slots[idx] = NULL;
    f->type_tags[idx] = -1;
}

void stackframe_set_type_tag(StackFrame* f, const char* name, int type_tag)
{
    if(!f || !name) return;
    int idx = frame_find(f, name);
    if(idx >= 0) {
        f->type_tags[idx] = type_tag;
    }
}

int stackframe_get_type_tag(StackFrame* f, const char* name)
{
    if(!f || !name) return -1;
    int idx = frame_find(f, name);
    if(idx >= 0) {
        return f->type_tags[idx];
    }
    return -1;
}

void stackframe_bind(StackFrame* f, const char* name, Value v)
{
    if(!f || !name) return;

    int idx = frame_find(f, name);
    if(idx >= 0) {
        slot_release(&f->vals[idx]);
        f->vals[idx] = v;
        return;
    }

    frame_ensure(f, f->cnt + 1);
    idx = f->cnt++;
    f->names[idx] = strdup(name);
    f->vals[idx] = v;
    f->int_slots[idx] = 0;
    f->flt_slots[idx] = 0.0;
    f->ptr_slots[idx] = NULL;
    f->type_tags[idx] = -1;
}

/* ===== 宽槽专用访问（int64 / double / ptr） ===== */

int64_t stackframe_get_int64(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0;

    StackFrame* cur = f;
    while(cur) {
        int idx = frame_find(cur, name);
        if(idx >= 0) {
            if(found) *found = 1;
            return cur->int_slots[idx];
        }
        cur = cur->parent;
    }
    return 0;
}

void stackframe_bind_int64(StackFrame* f, const char* name, int64_t v)
{
    if(!f || !name) return;

    int idx = frame_find(f, name);
    if(idx >= 0) {
        f->int_slots[idx] = v;
        return;
    }

    frame_ensure(f, f->cnt + 1);
    idx = f->cnt++;
    f->names[idx] = strdup(name);
    f->vals[idx].type = VAL_INT;
    f->vals[idx].v.i = v;
    f->int_slots[idx] = v;
    f->flt_slots[idx] = 0.0;
    f->ptr_slots[idx] = NULL;
    f->type_tags[idx] = -1;
}

double stackframe_get_double(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return 0;

    StackFrame* cur = f;
    while(cur) {
        int idx = frame_find(cur, name);
        if(idx >= 0) {
            if(found) *found = 1;
            return cur->flt_slots[idx];
        }
        cur = cur->parent;
    }
    return 0;
}

void stackframe_bind_double(StackFrame* f, const char* name, double v)
{
    if(!f || !name) return;

    int idx = frame_find(f, name);
    if(idx >= 0) {
        f->flt_slots[idx] = v;
        return;
    }

    frame_ensure(f, f->cnt + 1);
    idx = f->cnt++;
    f->names[idx] = strdup(name);
    f->vals[idx].type = VAL_DOUBLE;
    f->vals[idx].v.d = v;
    f->int_slots[idx] = 0;
    f->flt_slots[idx] = v;
    f->ptr_slots[idx] = NULL;
    f->type_tags[idx] = -1;
}

void* stackframe_get_ptr(StackFrame* f, const char* name, _Bool* found)
{
    if(found) *found = 0;
    if(!f || !name) return NULL;

    StackFrame* cur = f;
    while(cur) {
        int idx = frame_find(cur, name);
        if(idx >= 0) {
            if(found) *found = 1;
            return cur->ptr_slots[idx];
        }
        cur = cur->parent;
    }
    return NULL;
}

void stackframe_bind_ptr(StackFrame* f, const char* name, void* v)
{
    if(!f || !name) return;

    int idx = frame_find(f, name);
    if(idx >= 0) {
        f->ptr_slots[idx] = v;
        return;
    }

    frame_ensure(f, f->cnt + 1);
    idx = f->cnt++;
    f->names[idx] = strdup(name);
    f->vals[idx].type = VAL_PTR;
    f->vals[idx].v.struct_ptr = v;
    f->int_slots[idx] = 0;
    f->flt_slots[idx] = 0.0;
    f->ptr_slots[idx] = v;
    f->type_tags[idx] = -1;
}

/* ===== 闭包单元（cell）支持 ===== */

void stackframe_add_cell(StackFrame* f, const char* name, Value* cell_ptr)
{
    if(!f || !name || !cell_ptr) return;
    if(f->cell_cnt >= f->cell_cap) {
        f->cell_cap = f->cell_cap > 0 ? f->cell_cap * 2 : 4;
        f->cell_names = (char**)realloc(f->cell_names, (size_t)f->cell_cap * sizeof(char*));
        f->cells = (Value**)realloc(f->cells, (size_t)f->cell_cap * sizeof(Value*));
    }
    f->cell_names[f->cell_cnt] = strdup(name);
    f->cells[f->cell_cnt] = cell_ptr;
    f->cell_cnt++;
}

Value* stackframe_ensure_cell(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;

    /* 先查 cell 表 */
    for(int i = 0; i < f->cell_cnt; i++) {
        if(f->cell_names[i] && strcmp(f->cell_names[i], name) == 0) {
            return f->cells[i];
        }
    }

    /* 查普通槽位，找到则装箱 */
    int idx = frame_find(f, name);
    if(idx >= 0) {
        Value* cell = (Value*)malloc(sizeof(Value));
        if(!cell) { perror("stackframe_ensure_cell"); exit(EXIT_FAILURE); }
        *cell = f->vals[idx];
        stackframe_add_cell(f, name, cell);
        return cell;
    }

    return NULL;
}

Value** stackframe_find_cell(StackFrame* f, const char* name)
{
    if(!f || !name) return NULL;
    for(int i = 0; i < f->cell_cnt; i++) {
        if(f->cell_names[i] && strcmp(f->cell_names[i], name) == 0) {
            return &f->cells[i];
        }
    }
    return NULL;
}
