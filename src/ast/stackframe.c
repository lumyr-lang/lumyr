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

    /* 扩容 int_vals：malloc + memcpy，更新指针后 free 旧缓冲区
       int_vals 存储声明为 int 类型的变量的原始 int 值，用于 OPC_LOAD_INT_VAR 零提取 */
    int* niv = (int*)malloc((size_t)newcap * sizeof(int));
    if(!niv) { perror("stackframe expand int_vals"); exit(EXIT_FAILURE); }
    if(f->int_vals) {
        memcpy(niv, f->int_vals, (size_t)f->cap * sizeof(int));
    }
    /* 新槽位初始化为 0 */
    for(int i = f->cap; i < newcap; i++) niv[i] = 0;
    int* old_int_vals = f->int_vals;
    f->int_vals = niv;
    free(old_int_vals);

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
    free(f->int_vals);
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
