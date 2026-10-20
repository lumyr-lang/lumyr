// lumyr_typed_maps.c —— 类型化字典实现（扁平化 8 块连续数组 + 索引版红黑树）
// 与 TypedArray 对齐：运行时按 key_type/value_type 分发
// 红黑树操作完全基于 int32_t 索引，类型无关；仅 hash/compare 按 key_type 分发
#include "lumyr_typed_maps.h"
#include "lm_map.h"
#include "lm_value.h"
#include "gc_runtime.h"
#include "lumyr_log.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* 从 Value 提取裸指针（PTR 族），与 vm_exec_stack.c value_to_typed_ptr 同构 */
static void* tmap_value_to_ptr(Value v) {
    switch(v.type) {
    case VAL_STRING:
        if(v.str_inline) {
            char* p = (char*)gc_alloc((size_t)v.v.sso.len + 1, VAL_STRING);
            memcpy(p, v.v.sso.data, (size_t)v.v.sso.len + 1);
            return p;
        }
        return v.v.s;
    case VAL_PTR:        return v.v.struct_ptr;
    case VAL_STRUCT_PTR:
    case VAL_CLASS_PTR:  return v.v.struct_ptr;
    case VAL_BIGINT:     return v.v.bigint;
    case VAL_DECIMAL:    return v.v.decimal;
    case VAL_BITDECIMAL: return v.v.bitdecimal;
    case VAL_MAP:        return v.v.map;
    case VAL_ARRAY:      return v.v.array;
    case VAL_TYPED_ARRAY: return v.v.typed_array;
    default: {
        char buf[40];
        int n;
        if(v.type == VAL_DOUBLE || v.type == VAL_FLOAT || v.type == VAL_LONG_DOUBLE)
            n = snprintf(buf, sizeof(buf), "%g", v.v.d);
        else
            n = snprintf(buf, sizeof(buf), "%lld", (long long)lumyr_extract_ll(v));
        if(n < 0) n = 0;
        char* p = (char*)gc_alloc((size_t)n + 1, VAL_STRING);
        memcpy(p, buf, (size_t)n + 1);
        return p;
    }
    }
}

/* ==================== 类型分发辅助 ==================== */

/* 按 key_type 比较裸 key（从 keys 数组指定槽位读取），返回 -1/0/1 */
static int tmap_cmp_key(ValueType kt, const void* keys, int idx, Value query) {
    int cls = lumyr_etype_stackcls(kt);
    if(cls == 1) {
        /* 整数族：跨子类型按 i64 值比较（与 key_compare 一致） */
        int64_t a = 0;
        switch(kt) {
            case VAL_INT: case VAL_UINT: case VAL_FLOAT:
            case VAL_INT32: case VAL_UINT32:
                a = (int64_t)((int32_t*)keys)[idx]; break;
            case VAL_INT8: case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE:
            case VAL_BOOL: case VAL_CHAR:
                a = (int64_t)((int8_t*)keys)[idx]; break;
            case VAL_INT16: case VAL_UINT16: case VAL_SHORT: case VAL_USHORT:
                a = (int64_t)((int16_t*)keys)[idx]; break;
            case VAL_INT64: case VAL_UINT64: case VAL_LONG: case VAL_ULONG:
            case VAL_SIZE_T: case VAL_SSIZE_T: case VAL_LONG_LONG:
                a = ((int64_t*)keys)[idx]; break;
            default: a = (int64_t)((int32_t*)keys)[idx]; break;
        }
        int64_t b = lumyr_extract_ll(query);
        return (a > b) - (a < b);
    }
    if(cls == 2) {
        double a = 0;
        if(kt == VAL_DOUBLE) a = ((double*)keys)[idx];
        else if(kt == VAL_FLOAT) a = (double)((float*)keys)[idx];
        else a = (double)((long double*)keys)[idx];
        double b = 0;
        switch(query.type) {
            case VAL_DOUBLE: b = query.v.d; break;
            case VAL_FLOAT: b = (double)query.v.f; break;
            case VAL_LONG_DOUBLE: b = (double)query.v.ld; break;
            default: b = (double)lumyr_extract_ll(query); break;
        }
        return (a > b) - (a < b);
    }
    /* PTR 族 */
    void* a = ((void**)keys)[idx];
    if(kt == VAL_STRING) {
        const char* sa = a ? (const char*)a : "";
        const char* sb = lumyr_str_cstr(&query) ? lumyr_str_cstr(&query) : "";
        int c = strcmp(sa, sb);
        return (c > 0) - (c < 0);
    }
    /* 其他 PTR：指针身份比较 */
    uintptr_t pa = (uintptr_t)a;
    uintptr_t pb = (uintptr_t)query.v.s;
    return (pa > pb) - (pa < pb);
}

/* 从类型化数组读出 Value（用于 keys()/values() 和 GC 标记） */
static Value tmap_read_value(ValueType et, const void* arr, int idx) {
    Value v;
    memset(&v, 0, sizeof(v));
    v.type = et;
    int cls = lumyr_etype_stackcls(et);
    if(cls == 1) {
        switch(et) {
            case VAL_INT: v.v.i = (int)((int32_t*)arr)[idx]; break;
            case VAL_INT8: v.v.i8 = ((int8_t*)arr)[idx]; break;
            case VAL_INT16: v.v.i16 = ((int16_t*)arr)[idx]; break;
            case VAL_INT32: v.v.i32 = ((int32_t*)arr)[idx]; break;
            case VAL_INT64: case VAL_LONG_LONG: v.v.i64 = ((int64_t*)arr)[idx]; break;
            case VAL_LONG: v.v.l = ((long*)arr)[idx]; break;
            case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE: v.v.u8 = ((uint8_t*)arr)[idx]; break;
            case VAL_UINT16: case VAL_USHORT: v.v.u16 = ((uint16_t*)arr)[idx]; break;
            case VAL_UINT32: v.v.u32 = ((uint32_t*)arr)[idx]; break;
            case VAL_UINT: v.v.ui = ((unsigned int*)arr)[idx]; break;
            case VAL_UINT64: v.v.u64 = ((uint64_t*)arr)[idx]; break;
            case VAL_ULONG: v.v.ul = ((unsigned long*)arr)[idx]; break;
            case VAL_SIZE_T: v.v.st = ((size_t*)arr)[idx]; break;
            case VAL_SSIZE_T: v.v.sst = ((ssize_t*)arr)[idx]; break;
            case VAL_BOOL: v.v.b = ((_Bool*)arr)[idx]; break;
            case VAL_CHAR: v.v.c = ((char*)arr)[idx]; break;
            default: v.v.i64 = ((int64_t*)arr)[idx]; break;
        }
    } else if(cls == 2) {
        if(et == VAL_DOUBLE) v.v.d = ((double*)arr)[idx];
        else if(et == VAL_FLOAT) v.v.f = ((float*)arr)[idx];
        else v.v.ld = ((long double*)arr)[idx];
    } else {
        /* PTR 族 */
        v.str_inline = 0;
        v.v.s = (char*)((void**)arr)[idx];
    }
    return v;
}

/* 把 Value 写入类型化数组指定槽位（从 Value 提取并按 et 存储） */
static void tmap_write_value(ValueType et, void* arr, int idx, Value v) {
    int cls = lumyr_etype_stackcls(et);
    if(cls == 1) {
        int64_t val = lumyr_extract_ll(v);
        switch(et) {
            case VAL_INT: ((int32_t*)arr)[idx] = (int32_t)val; break;
            case VAL_INT8: ((int8_t*)arr)[idx] = (int8_t)val; break;
            case VAL_INT16: ((int16_t*)arr)[idx] = (int16_t)val; break;
            case VAL_INT32: ((int32_t*)arr)[idx] = (int32_t)val; break;
            case VAL_INT64: case VAL_LONG_LONG: ((int64_t*)arr)[idx] = val; break;
            case VAL_LONG: ((long*)arr)[idx] = (long)val; break;
            case VAL_UINT8: case VAL_UCHAR: case VAL_BYTE: ((uint8_t*)arr)[idx] = (uint8_t)val; break;
            case VAL_UINT16: case VAL_USHORT: ((uint16_t*)arr)[idx] = (uint16_t)val; break;
            case VAL_UINT32: ((uint32_t*)arr)[idx] = (uint32_t)val; break;
            case VAL_UINT: ((unsigned int*)arr)[idx] = (unsigned int)val; break;
            case VAL_UINT64: ((uint64_t*)arr)[idx] = (uint64_t)val; break;
            case VAL_ULONG: ((unsigned long*)arr)[idx] = (unsigned long)val; break;
            case VAL_SIZE_T: ((size_t*)arr)[idx] = (size_t)val; break;
            case VAL_SSIZE_T: ((ssize_t*)arr)[idx] = (ssize_t)val; break;
            case VAL_BOOL: ((_Bool*)arr)[idx] = (_Bool)(val != 0); break;
            case VAL_CHAR: ((char*)arr)[idx] = (char)val; break;
            default: ((int64_t*)arr)[idx] = val; break;
        }
    } else if(cls == 2) {
        double val = 0;
        switch(v.type) {
            case VAL_DOUBLE: val = v.v.d; break;
            case VAL_FLOAT: val = (double)v.v.f; break;
            case VAL_LONG_DOUBLE: val = (double)v.v.ld; break;
            default: val = (double)lumyr_extract_ll(v); break;
        }
        if(et == VAL_DOUBLE) ((double*)arr)[idx] = val;
        else if(et == VAL_FLOAT) ((float*)arr)[idx] = (float)val;
        else ((long double*)arr)[idx] = (long double)val;
    } else {
        /* PTR 族 */
        void* p = NULL;
        if(et == VAL_STRING) {
            /* 字符串落独立 GC 副本（与 TypedArray string 处理一致），但只拷一次：
             * SSO 数据在栈上 Value 内、直接从 SSO 缓冲区拷；堆串从堆指针拷。
             * 不走 tmap_value_to_ptr——它会对 SSO 预拷一份，
             * 旧路径每条字符串两次 gc_alloc，第一份立即成为垃圾。
             * 非标量族 Value（如 int 写入 <K,string>map）仍走 tmap_value_to_ptr
             * 格式化为字符串，保留标量→string 强转语义。 */
            const char* src;
            size_t l;
            if(v.type == VAL_STRING) {
                if(v.str_inline) { src = v.v.sso.data; l = (size_t)v.v.sso.len; }
                else { src = v.v.s ? v.v.s : ""; l = strlen(src); }
            } else {
                src = (const char*)tmap_value_to_ptr(v);
                if(!src) src = "";
                l = strlen(src);
            }
            char* np = (char*)gc_alloc(l + 1, VAL_STRING);
            memcpy(np, src, l + 1);
            p = np;
        } else {
            p = tmap_value_to_ptr(v);
        }
        ((void**)arr)[idx] = p;
    }
}

/* ==================== 索引版红黑树操作（类型无关，仅用 int32_t 索引） ==================== */

static int32_t tmap_bucket_idx(uint32_t hash, int cap) {
    return (int32_t)(hash & (uint32_t)(cap - 1));
}

/* 红黑树查找：返回槽位索引或 -1 */
static int32_t tmap_tree_find(TypedMap* m, int bucket, Value key, uint32_t h) {
    int32_t cur = m->buckets[bucket];
    while(cur != -1) {
        if(m->hashes[cur] == h) {
            int cmp = tmap_cmp_key(m->key_type, m->keys, cur, key);
            if(cmp == 0) return cur;
            cur = (cmp < 0) ? m->right[cur] : m->left[cur];
        } else {
            cur = (m->hashes[cur] > h) ? m->left[cur] : m->right[cur];
        }
    }
    return -1;
}

/* 红黑树左旋 */
static void tmap_rotate_left(TypedMap* m, int bucket, int32_t x) {
    int32_t y = m->right[x];
    m->right[x] = m->left[y];
    if(m->left[y] != -1) m->parent[m->left[y]] = x;
    m->parent[y] = m->parent[x];
    if(m->parent[x] == -1) m->buckets[bucket] = y;
    else if(x == m->left[m->parent[x]]) m->left[m->parent[x]] = y;
    else m->right[m->parent[x]] = y;
    m->left[y] = x;
    m->parent[x] = y;
}

/* 红黑树右旋 */
static void tmap_rotate_right(TypedMap* m, int bucket, int32_t x) {
    int32_t y = m->left[x];
    m->left[x] = m->right[y];
    if(m->right[y] != -1) m->parent[m->right[y]] = x;
    m->parent[y] = m->parent[x];
    if(m->parent[x] == -1) m->buckets[bucket] = y;
    else if(x == m->right[m->parent[x]]) m->right[m->parent[x]] = y;
    else m->left[m->parent[x]] = y;
    m->right[y] = x;
    m->parent[x] = y;
}

/* 红黑树插入后平衡 */
static void tmap_insert_fixup(TypedMap* m, int bucket, int32_t z) {
    while(m->parent[z] != -1 && m->rb_color[m->parent[z]] == TMAP_RED) {
        int32_t pz = m->parent[z];
        int32_t ppz = m->parent[pz];
        if(ppz == -1) break;
        if(pz == m->left[ppz]) {
            int32_t y = m->right[ppz];
            if(y != -1 && m->rb_color[y] == TMAP_RED) {
                m->rb_color[pz] = TMAP_BLACK;
                m->rb_color[y] = TMAP_BLACK;
                m->rb_color[ppz] = TMAP_RED;
                z = ppz;
            } else {
                if(z == m->right[pz]) { z = pz; tmap_rotate_left(m, bucket, z); }
                int32_t pz2 = m->parent[z];
                int32_t ppz2 = m->parent[pz2];
                m->rb_color[pz2] = TMAP_BLACK;
                if(ppz2 != -1) {
                    m->rb_color[ppz2] = TMAP_RED;
                    tmap_rotate_right(m, bucket, ppz2);
                }
            }
        } else {
            int32_t y = m->left[ppz];
            if(y != -1 && m->rb_color[y] == TMAP_RED) {
                m->rb_color[pz] = TMAP_BLACK;
                m->rb_color[y] = TMAP_BLACK;
                m->rb_color[ppz] = TMAP_RED;
                z = ppz;
            } else {
                if(z == m->left[pz]) { z = pz; tmap_rotate_right(m, bucket, z); }
                int32_t pz2 = m->parent[z];
                int32_t ppz2 = m->parent[pz2];
                m->rb_color[pz2] = TMAP_BLACK;
                if(ppz2 != -1) {
                    m->rb_color[ppz2] = TMAP_RED;
                    tmap_rotate_left(m, bucket, ppz2);
                }
            }
        }
    }
    m->rb_color[m->buckets[bucket]] = TMAP_BLACK;
}

/* 红黑树插入 */
static void tmap_tree_insert(TypedMap* m, int bucket, int32_t z, uint32_t h) {
    int32_t y = -1;
    int32_t x = m->buckets[bucket];
    while(x != -1) {
        y = x;
        if(m->hashes[x] != h) {
            x = (m->hashes[x] > h) ? m->left[x] : m->right[x];
        } else {
            int cmp = tmap_cmp_key(m->key_type, m->keys, x, tmap_read_value(m->key_type, m->keys, z));
            x = (cmp < 0) ? m->right[x] : m->left[x];
        }
    }
    m->parent[z] = y;
    if(y == -1) m->buckets[bucket] = z;
    else {
        if(m->hashes[y] != h) {
            if(m->hashes[y] > h) m->left[y] = z;
            else m->right[y] = z;
        } else {
            int cmp = tmap_cmp_key(m->key_type, m->keys, y, tmap_read_value(m->key_type, m->keys, z));
            if(cmp < 0) m->right[y] = z;
            else m->left[y] = z;
        }
    }
    m->rb_color[z] = TMAP_RED;
    m->left[z] = m->right[z] = -1;
    tmap_insert_fixup(m, bucket, z);
}

/* 红黑树最小节点 */
static int32_t tmap_tree_min(TypedMap* m, int32_t x) {
    while(m->left[x] != -1) x = m->left[x];
    return x;
}

/* 红黑树删除后平衡 */
static void tmap_delete_fixup(TypedMap* m, int bucket, int32_t x, int32_t x_parent) {
    while(x != m->buckets[bucket] && (x == -1 || m->rb_color[x] == TMAP_BLACK)) {
        if(x_parent == -1) break;
        if(x == m->left[x_parent]) {
            int32_t w = m->right[x_parent];
            if(w != -1 && m->rb_color[w] == TMAP_RED) {
                m->rb_color[w] = TMAP_BLACK;
                m->rb_color[x_parent] = TMAP_RED;
                tmap_rotate_left(m, bucket, x_parent);
                w = m->right[x_parent];
            }
            int wl_black = (m->left[w] == -1 || m->rb_color[m->left[w]] == TMAP_BLACK);
            int wr_black = (m->right[w] == -1 || m->rb_color[m->right[w]] == TMAP_BLACK);
            if(wl_black && wr_black) {
                if(w != -1) m->rb_color[w] = TMAP_RED;
                x = x_parent;
                x_parent = m->parent[x];
            } else {
                if(wr_black) {
                    if(m->left[w] != -1) m->rb_color[m->left[w]] = TMAP_BLACK;
                    if(w != -1) m->rb_color[w] = TMAP_RED;
                    tmap_rotate_right(m, bucket, w);
                    w = m->right[x_parent];
                }
                if(w != -1) m->rb_color[w] = m->rb_color[x_parent];
                m->rb_color[x_parent] = TMAP_BLACK;
                if(m->right[w] != -1) m->rb_color[m->right[w]] = TMAP_BLACK;
                tmap_rotate_left(m, bucket, x_parent);
                x = m->buckets[bucket];
                break;
            }
        } else {
            int32_t w = m->left[x_parent];
            if(w != -1 && m->rb_color[w] == TMAP_RED) {
                m->rb_color[w] = TMAP_BLACK;
                m->rb_color[x_parent] = TMAP_RED;
                tmap_rotate_right(m, bucket, x_parent);
                w = m->left[x_parent];
            }
            int wr_black = (m->right[w] == -1 || m->rb_color[m->right[w]] == TMAP_BLACK);
            int wl_black = (m->left[w] == -1 || m->rb_color[m->left[w]] == TMAP_BLACK);
            if(wr_black && wl_black) {
                if(w != -1) m->rb_color[w] = TMAP_RED;
                x = x_parent;
                x_parent = m->parent[x];
            } else {
                if(wl_black) {
                    if(m->right[w] != -1) m->rb_color[m->right[w]] = TMAP_BLACK;
                    if(w != -1) m->rb_color[w] = TMAP_RED;
                    tmap_rotate_left(m, bucket, w);
                    w = m->left[x_parent];
                }
                if(w != -1) m->rb_color[w] = m->rb_color[x_parent];
                m->rb_color[x_parent] = TMAP_BLACK;
                if(m->left[w] != -1) m->rb_color[m->left[w]] = TMAP_BLACK;
                tmap_rotate_right(m, bucket, x_parent);
                x = m->buckets[bucket];
                break;
            }
        }
    }
    if(x != -1) m->rb_color[x] = TMAP_BLACK;
}

/* 红黑树删除节点 */
static void tmap_tree_remove(TypedMap* m, int bucket, int32_t z) {
    int32_t y = z;
    int32_t x, x_parent;
    int y_orig_color = m->rb_color[y];
    if(m->left[z] == -1) {
        x = m->right[z];
        x_parent = m->parent[z];
        if(m->parent[z] == -1) m->buckets[bucket] = m->right[z];
        else if(z == m->left[m->parent[z]]) m->left[m->parent[z]] = m->right[z];
        else m->right[m->parent[z]] = m->right[z];
        if(m->right[z] != -1) m->parent[m->right[z]] = m->parent[z];
    } else if(m->right[z] == -1) {
        x = m->left[z];
        x_parent = m->parent[z];
        if(m->parent[z] == -1) m->buckets[bucket] = m->left[z];
        else if(z == m->left[m->parent[z]]) m->left[m->parent[z]] = m->left[z];
        else m->right[m->parent[z]] = m->left[z];
        if(m->left[z] != -1) m->parent[m->left[z]] = m->parent[z];
    } else {
        y = tmap_tree_min(m, m->right[z]);
        y_orig_color = m->rb_color[y];
        x = m->right[y];
        if(m->parent[y] == z) {
            x_parent = y;
        } else {
            x_parent = m->parent[y];
            if(m->right[y] != -1) m->parent[m->right[y]] = m->parent[y];
            m->left[m->parent[y]] = m->right[y];
            m->right[y] = m->right[z];
            m->parent[m->right[y]] = y;
        }
        if(m->parent[z] == -1) m->buckets[bucket] = y;
        else if(z == m->left[m->parent[z]]) m->left[m->parent[z]] = y;
        else m->right[m->parent[z]] = y;
        m->parent[y] = m->parent[z];
        m->rb_color[y] = m->rb_color[z];
        m->left[y] = m->left[z];
        m->parent[m->left[y]] = y;
    }
    if(y_orig_color == TMAP_BLACK)
        tmap_delete_fixup(m, bucket, x, x_parent);
}

/* 红黑树节点数 */
static int tmap_tree_count(TypedMap* m, int32_t root) {
    if(root == -1) return 0;
    return 1 + tmap_tree_count(m, m->left[root]) + tmap_tree_count(m, m->right[root]);
}

/* 链表查找 */
static int32_t tmap_list_find(TypedMap* m, int bucket, Value key, uint32_t h) {
    int32_t cur = m->buckets[bucket];
    while(cur != -1) {
        if(m->hashes[cur] == h && tmap_cmp_key(m->key_type, m->keys, cur, key) == 0)
            return cur;
        cur = m->next[cur];
    }
    return -1;
}

static int tmap_list_count(TypedMap* m, int bucket) {
    int n = 0;
    int32_t cur = m->buckets[bucket];
    while(cur != -1) { n++; cur = m->next[cur]; }
    return n;
}

/* 链表 -> 红黑树 */
static void tmap_treeify_bin(TypedMap* m, int bucket) {
    if(m->cap < TMAP_MIN_TREEIFY_CAP) return;
    int32_t head = m->buckets[bucket];
    if(head == -1) return;
    /* 先清空桶 */
    m->buckets[bucket] = -1;
    int32_t e = head;
    while(e != -1) {
        int32_t next = m->next[e];
        m->left[e] = m->right[e] = m->parent[e] = -1;
        m->next[e] = -1;
        tmap_tree_insert(m, bucket, e, m->hashes[e]);
        e = next;
    }
    m->tree[bucket] = 1;
}

/* 红黑树 -> 链表（中序遍历） */
static void tmap_untreeify_bin(TypedMap* m, int bucket) {
    int32_t root = m->buckets[bucket];
    if(root == -1) return;
    int32_t head = -1;
    int32_t tail = -1;
    /* 非递归中序遍历 */
    int32_t stk[256];
    int top = 0;
    int32_t cur = root;
    while(cur != -1 || top > 0) {
        while(cur != -1) {
            if(top < 256) stk[top++] = cur;
            cur = m->left[cur];
        }
        if(top == 0) break;
        cur = stk[--top];
        int32_t next = m->right[cur];
        m->left[cur] = m->right[cur] = m->parent[cur] = -1;
        m->next[cur] = -1;
        if(head == -1) head = cur;
        else m->next[tail] = cur;
        tail = cur;
        cur = next;
    }
    m->buckets[bucket] = head;
    m->tree[bucket] = 0;
}

/* ==================== 扩容 rehash ==================== */
static void tmap_resize(TypedMap* m) {
    int old_cap = m->cap;
    int new_cap = old_cap * 2;
    size_t keysz = lumyr_etype_itemsz(m->key_type);
    size_t valsz = lumyr_etype_itemsz(m->value_type);

    void* new_keys = gc_alloc_old(keysz * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_keys);
    void* new_values = gc_alloc_old(valsz * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_values);
    uint32_t* new_hashes = (uint32_t*)gc_alloc_old(sizeof(uint32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_hashes);
    int32_t* new_next = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_next);
    int32_t* new_left = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_left);
    int32_t* new_right = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_right);
    int32_t* new_parent = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_parent);
    int8_t* new_color = (int8_t*)gc_alloc_old(sizeof(int8_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_color);
    int32_t* new_buckets = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_buckets);
    unsigned char* new_tree = (unsigned char*)gc_alloc_old(sizeof(unsigned char) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(new_tree);

    memcpy(new_keys, m->keys, keysz * (size_t)old_cap);
    memcpy(new_values, m->values, valsz * (size_t)old_cap);
    memcpy(new_hashes, m->hashes, sizeof(uint32_t) * (size_t)old_cap);
    memcpy(new_next, m->next, sizeof(int32_t) * (size_t)old_cap);
    memcpy(new_left, m->left, sizeof(int32_t) * (size_t)old_cap);
    memcpy(new_right, m->right, sizeof(int32_t) * (size_t)old_cap);
    memcpy(new_parent, m->parent, sizeof(int32_t) * (size_t)old_cap);
    memcpy(new_color, m->rb_color, sizeof(int8_t) * (size_t)old_cap);
    memset(new_buckets, 0xFF, sizeof(int32_t) * (size_t)new_cap); /* -1 = 空桶 */
    memset(new_tree, 0, sizeof(unsigned char) * (size_t)new_cap);

    /* 重建链表和树结构（rehash 到新桶） */
    for(int i = 0; i < new_cap; i++) new_buckets[i] = -1;
    for(int i = 0; i < old_cap; i++) new_next[i] = -1;
    for(int i = 0; i < old_cap; i++) new_left[i] = new_right[i] = new_parent[i] = -1;

    for(int i = 0; i < m->len; i++) {
        if(m->next[i] == -2) continue; /* 空闲槽位跳过 */
        int ni = tmap_bucket_idx(new_hashes[i], new_cap);
        new_next[i] = new_buckets[ni];
        new_buckets[ni] = i;
    }

    m->keys = new_keys;
    m->values = new_values;
    m->hashes = new_hashes;
    m->next = new_next;
    m->left = new_left;
    m->right = new_right;
    m->parent = new_parent;
    m->rb_color = new_color;
    m->buckets = new_buckets;
    m->tree = new_tree;
    m->cap = new_cap;
}

/* ==================== 公共 API ==================== */

TypedMap* lumyr_tmap_new(ValueType key_type, ValueType value_type, int cap) {
    if(cap <= 0) cap = TMAP_INIT_CAP;
    /* 确保 cap 是 2 的幂 */
    int c = 1;
    while(c < cap) c <<= 1;
    cap = c;

    TypedMap* m = (TypedMap*)gc_alloc(sizeof(TypedMap), VAL_TYPED_MAP);
    if(!m) { LOG_ERROR("TypedMap 分配失败\n"); exit(EXIT_FAILURE); }
    memset(m, 0, sizeof(TypedMap));
    m->key_type = key_type;
    m->value_type = value_type;
    m->cap = cap;
    m->len = 0;
    m->stack_alloc = 0;
    m->write_flag = 0;
    m->free_head = -1;
    m->free_count = 0;

    size_t keysz = lumyr_etype_itemsz(key_type);
    size_t valsz = lumyr_etype_itemsz(value_type);
    m->keys = gc_alloc_old(keysz * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->keys);
    m->values = gc_alloc_old(valsz * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->values);
    m->hashes = (uint32_t*)gc_alloc_old(sizeof(uint32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->hashes);
    m->next = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->next);
    m->left = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->left);
    m->right = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->right);
    m->parent = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->parent);
    m->rb_color = (int8_t*)gc_alloc_old(sizeof(int8_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->rb_color);
    m->buckets = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->buckets);
    m->tree = (unsigned char*)gc_alloc_old(sizeof(unsigned char) * (size_t)cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(m->tree);

    /* 初始化桶为 -1 */
    for(int i = 0; i < cap; i++) {
        m->buckets[i] = -1;
        m->next[i] = -1;
        m->left[i] = m->right[i] = m->parent[i] = -1;
        m->tree[i] = 0;
    }

    return m;
}

TypedMap* lumyr_tmap_from_value_map(ValueMap* vm, ValueType key_type, ValueType value_type) {
    if(!vm) return lumyr_tmap_new(key_type, value_type, TMAP_INIT_CAP);
    TypedMap* m = lumyr_tmap_new(key_type, value_type, vm->cap > 0 ? vm->cap : TMAP_INIT_CAP);
    /* 遍历 ValueMap 逐条写入；期间禁止 GC，防止迭代栈上的 k/v 不是根
     * 而被移动/回收，导致 lumyr_tmap_set 的 hash/compare 读到脏 Value */
    gc_disable();
    MapIter it;
    map_iter_init(&it, vm);
    Value k, v;
    while(map_iter_next(&it, &k, &v)) {
        lumyr_tmap_set(m, k, v);
    }
    gc_enable();
    return m;
}

void lumyr_tmap_set(TypedMap* m, Value key, Value val) {
    if(!m) return;
    /* 用 Value 的哈希（与 VAL_MAP 一致的跨子类型相等契约） */
    uint32_t h = 0;
    {
        int cls = lumyr_etype_stackcls(m->key_type);
        if(cls == 1) {
            h = (uint32_t)(lumyr_extract_ll(key) * 2654435761u);
        } else if(cls == 2) {
            double d = 0;
            switch(key.type) {
                case VAL_DOUBLE: d = key.v.d; break;
                case VAL_FLOAT: d = (double)key.v.f; break;
                case VAL_LONG_DOUBLE: d = (double)key.v.ld; break;
                default: d = (double)lumyr_extract_ll(key); break;
            }
            uint64_t bits; memcpy(&bits, &d, sizeof(double));
            h = (uint32_t)(bits ^ (bits >> 32) ^ 2654435761u);
        } else {
            if(m->key_type == VAL_STRING) {
                const char* s = lumyr_str_cstr(&key) ? lumyr_str_cstr(&key) : "";
                h = 5381;
                while(*s) h = ((h << 5) + h) + (unsigned char)*s++;
            } else {
                uintptr_t p = (uintptr_t)key.v.s;
                h = (uint32_t)(p ^ (p >> 32) ^ 2654435761u);
            }
        }
        h ^= (h >> 16);
    }

    int bucket = tmap_bucket_idx(h, m->cap);

    /* 查找现有 key */
    int32_t slot = -1;
    if(m->tree[bucket]) {
        slot = tmap_tree_find(m, bucket, key, h);
    } else {
        slot = tmap_list_find(m, bucket, key, h);
    }

    if(slot != -1) {
        /* 更新 value */
        gc_write_barrier(val);
        tmap_write_value(m->value_type, m->values, slot, val);
        return;
    }

    /* 新增：优先从 free_head 复用 */
    if(m->free_head != -1) {
        slot = m->free_head;
        m->free_head = m->next[slot];
        m->free_count--;
    } else {
        /* 需要扩容 */
        if(m->len >= m->cap) {
            tmap_resize(m);
            bucket = tmap_bucket_idx(h, m->cap);
        }
        slot = m->len;
    }

    /* 写入 key/value/hash */
    tmap_write_value(m->key_type, m->keys, slot, key);
    gc_write_barrier(val);
    tmap_write_value(m->value_type, m->values, slot, val);
    m->hashes[slot] = h;
    m->next[slot] = -1;
    m->left[slot] = m->right[slot] = m->parent[slot] = -1;

    /* 插入到桶 */
    if(m->tree[bucket]) {
        tmap_tree_insert(m, bucket, slot, h);
    } else {
        m->next[slot] = m->buckets[bucket];
        m->buckets[bucket] = slot;
        /* 检查是否需要树化 */
        if(tmap_list_count(m, bucket) >= TMAP_TREEIFY_THRESHOLD)
            tmap_treeify_bin(m, bucket);
    }

    m->len++;
    /* 检查是否需要扩容 */
    if(m->len > (int)(m->cap * TMAP_LOAD_FACTOR)) {
        tmap_resize(m);
    }
}

Value lumyr_tmap_get(TypedMap* m, Value key) {
    if(!m) return val_none();
    /* 计算 hash（与 set 一致） */
    uint32_t h = 0;
    {
        int cls = lumyr_etype_stackcls(m->key_type);
        if(cls == 1) {
            h = (uint32_t)(lumyr_extract_ll(key) * 2654435761u);
        } else if(cls == 2) {
            double d = 0;
            switch(key.type) {
                case VAL_DOUBLE: d = key.v.d; break;
                case VAL_FLOAT: d = (double)key.v.f; break;
                case VAL_LONG_DOUBLE: d = (double)key.v.ld; break;
                default: d = (double)lumyr_extract_ll(key); break;
            }
            uint64_t bits; memcpy(&bits, &d, sizeof(double));
            h = (uint32_t)(bits ^ (bits >> 32) ^ 2654435761u);
        } else {
            if(m->key_type == VAL_STRING) {
                const char* s = lumyr_str_cstr(&key) ? lumyr_str_cstr(&key) : "";
                h = 5381;
                while(*s) h = ((h << 5) + h) + (unsigned char)*s++;
            } else {
                uintptr_t p = (uintptr_t)key.v.s;
                h = (uint32_t)(p ^ (p >> 32) ^ 2654435761u);
            }
        }
        h ^= (h >> 16);
    }

    int bucket = tmap_bucket_idx(h, m->cap);
    int32_t slot = -1;
    if(m->tree[bucket]) {
        slot = tmap_tree_find(m, bucket, key, h);
    } else {
        slot = tmap_list_find(m, bucket, key, h);
    }
    if(slot == -1) return val_none();
    return tmap_read_value(m->value_type, m->values, slot);
}

int lumyr_tmap_has(TypedMap* m, Value key) {
    if(!m) return 0;
    Value v = lumyr_tmap_get(m, key);
    return v.type != VAL_NONE;
}

void lumyr_tmap_del(TypedMap* m, Value key) {
    if(!m) return;
    /* 计算 hash */
    uint32_t h = 0;
    {
        int cls = lumyr_etype_stackcls(m->key_type);
        if(cls == 1) {
            h = (uint32_t)(lumyr_extract_ll(key) * 2654435761u);
        } else if(cls == 2) {
            double d = 0;
            switch(key.type) {
                case VAL_DOUBLE: d = key.v.d; break;
                case VAL_FLOAT: d = (double)key.v.f; break;
                case VAL_LONG_DOUBLE: d = (double)key.v.ld; break;
                default: d = (double)lumyr_extract_ll(key); break;
            }
            uint64_t bits; memcpy(&bits, &d, sizeof(double));
            h = (uint32_t)(bits ^ (bits >> 32) ^ 2654435761u);
        } else {
            if(m->key_type == VAL_STRING) {
                const char* s = lumyr_str_cstr(&key) ? lumyr_str_cstr(&key) : "";
                h = 5381;
                while(*s) h = ((h << 5) + h) + (unsigned char)*s++;
            } else {
                uintptr_t p = (uintptr_t)key.v.s;
                h = (uint32_t)(p ^ (p >> 32) ^ 2654435761u);
            }
        }
        h ^= (h >> 16);
    }

    int bucket = tmap_bucket_idx(h, m->cap);
    int32_t slot = -1;
    if(m->tree[bucket]) {
        slot = tmap_tree_find(m, bucket, key, h);
        if(slot != -1) {
            tmap_tree_remove(m, bucket, slot);
            /* 检查退化 */
            if(tmap_tree_count(m, m->buckets[bucket]) < TMAP_UNTREEIFY_THRESHOLD)
                tmap_untreeify_bin(m, bucket);
        }
    } else {
        /* 链表删除 */
        int32_t prev = -1;
        int32_t cur = m->buckets[bucket];
        while(cur != -1) {
            if(m->hashes[cur] == h && tmap_cmp_key(m->key_type, m->keys, cur, key) == 0) {
                if(prev == -1) m->buckets[bucket] = m->next[cur];
                else m->next[prev] = m->next[cur];
                slot = cur;
                break;
            }
            prev = cur;
            cur = m->next[cur];
        }
    }

    if(slot == -1) return;

    /* 挂到 free_head 链 */
    m->next[slot] = m->free_head;
    m->free_head = slot;
    m->free_count++;
    m->len--;

    /* free 槽位占比超 60% 时主动 compact，回收删除遗留槽位（对齐 3.10 策略） */
    if(m->free_count * 100 > m->cap * 60) lumyr_tmap_compact(m);
}

Value lumyr_tmap_keys(TypedMap* m) {
    if(!m) return val_none();
    ValueArray* arr = (ValueArray*)gc_alloc(sizeof(ValueArray), VAL_ARRAY);
    memset(arr, 0, sizeof(ValueArray));
    arr->items = (Value*)gc_alloc(sizeof(Value) * (size_t)(m->len > 0 ? m->len : 1), VAL_ARRAY);
    gc_mark_internal_buf(arr->items);
    arr->len = 0;
    arr->cap = m->len > 0 ? m->len : 1;

    for(int i = 0; i < m->cap && arr->len < m->len; i++) {
        int32_t cur = m->buckets[i];
        if(m->tree[i]) {
            /* 红黑树中序遍历 */
            int32_t stk[256];
            int top = 0;
            int32_t node = cur;
            while(node != -1 || top > 0) {
                while(node != -1) {
                    if(top < 256) stk[top++] = node;
                    node = m->left[node];
                }
                if(top == 0) break;
                node = stk[--top];
                arr->items[arr->len++] = tmap_read_value(m->key_type, m->keys, node);
                node = m->right[node];
            }
        } else {
            while(cur != -1) {
                arr->items[arr->len++] = tmap_read_value(m->key_type, m->keys, cur);
                cur = m->next[cur];
            }
        }
    }

    Value r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_ARRAY;
    r.v.array = arr;
    return r;
}

Value lumyr_tmap_values(TypedMap* m) {
    if(!m) return val_none();
    ValueArray* arr = (ValueArray*)gc_alloc(sizeof(ValueArray), VAL_ARRAY);
    memset(arr, 0, sizeof(ValueArray));
    arr->items = (Value*)gc_alloc(sizeof(Value) * (size_t)(m->len > 0 ? m->len : 1), VAL_ARRAY);
    gc_mark_internal_buf(arr->items);
    arr->len = 0;
    arr->cap = m->len > 0 ? m->len : 1;

    for(int i = 0; i < m->cap && arr->len < m->len; i++) {
        int32_t cur = m->buckets[i];
        if(m->tree[i]) {
            int32_t stk[256];
            int top = 0;
            int32_t node = cur;
            while(node != -1 || top > 0) {
                while(node != -1) {
                    if(top < 256) stk[top++] = node;
                    node = m->left[node];
                }
                if(top == 0) break;
                node = stk[--top];
                arr->items[arr->len++] = tmap_read_value(m->value_type, m->values, node);
                node = m->right[node];
            }
        } else {
            while(cur != -1) {
                arr->items[arr->len++] = tmap_read_value(m->value_type, m->values, cur);
                cur = m->next[cur];
            }
        }
    }

    Value r;
    memset(&r, 0, sizeof(r));
    r.type = VAL_ARRAY;
    r.v.array = arr;
    return r;
}

int lumyr_tmap_len(TypedMap* m) {
    return m ? m->len : 0;
}

/* TypedMap → 动态 ValueMap（键值装箱为 Value，供 toMap/json/copy 复用） */
Value lumyr_tmap_to_value_map(TypedMap* m) {
    Value r = val_map();
    if(!m) return r;
    Value ks = lumyr_tmap_keys(m);
    ValueArray* ka = ks.v.array;
    int n = ka ? ka->len : 0;
    for(int i = 0; i < n; i++) {
        Value k = ka->items[i];
        lumyr_map_set(&r, k, lumyr_tmap_get(m, k));
    }
    return r;
}

/* compact 第二遍：把旧槽位 old 的键/值/哈希写到紧凑下标 ni，并重建新桶链 */
static void tmap_compact_place(TypedMap* m, int32_t old, int32_t ni,
                               void* nk, void* nv, uint32_t* nh, int32_t* nn, int32_t* nb,
                               int new_cap, size_t keysz, size_t valsz) {
    memcpy((char*)nk + (size_t)ni * keysz, (const char*)m->keys + (size_t)old * keysz, keysz);
    memcpy((char*)nv + (size_t)ni * valsz, (const char*)m->values + (size_t)old * valsz, valsz);
    nh[ni] = m->hashes[old];
    int bucket = tmap_bucket_idx(nh[ni], new_cap);
    nn[ni] = nb[bucket];
    nb[bucket] = ni;
}

/* 主动 compact：遍历桶（链表 + 红黑树）枚举存活槽位，顺序重排到 [0, len)
 * 并重建桶索引；负载因子允许时缩容 cap。free 槽位与 free_head 链随之清空。 */
void lumyr_tmap_compact(TypedMap* m) {
    if(!m || m->free_count == 0) return;
    int live = m->len;

    /* 目标容量：满足负载因子、不小于初始容量、不超过当前容量（避免刚整理又扩容抖动） */
    int new_cap = TMAP_INIT_CAP;
    while((double)live > (double)new_cap * TMAP_LOAD_FACTOR) new_cap <<= 1;
    if(new_cap > m->cap) new_cap = m->cap;

    int32_t* remap = (int32_t*)malloc(sizeof(int32_t) * (size_t)m->cap);
    if(!remap) return;
    for(int i = 0; i < m->cap; i++) remap[i] = -1;

    /* 第一遍：按桶顺序给存活槽位分配紧凑下标 */
    int ns = 0;
    for(int b = 0; b < m->cap; b++) {
        int32_t cur = m->buckets[b];
        if(cur == -1) continue;
        if(m->tree[b]) {
            int32_t stk[256];
            int top = 0;
            int32_t node = cur;
            while(node != -1 || top > 0) {
                while(node != -1) {
                    if(top < 256) stk[top++] = node;
                    node = m->left[node];
                }
                if(top == 0) break;
                node = stk[--top];
                remap[node] = ns++;
                node = m->right[node];
            }
        } else {
            while(cur != -1) {
                remap[cur] = ns++;
                cur = m->next[cur];
            }
        }
    }
    if(ns != live) { free(remap); return; }   /* 结构不一致：放弃整理，保持原状 */

    size_t keysz = lumyr_etype_itemsz(m->key_type);
    size_t valsz = lumyr_etype_itemsz(m->value_type);
    void* nk = gc_alloc_old(keysz * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nk);
    void* nv = gc_alloc_old(valsz * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nv);
    uint32_t* nh = (uint32_t*)gc_alloc_old(sizeof(uint32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nh);
    int32_t* nn = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nn);
    int32_t* nl = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nl);
    int32_t* nr = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nr);
    int32_t* np = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(np);
    int8_t* ncol = (int8_t*)gc_alloc_old(sizeof(int8_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(ncol);
    int32_t* nb = (int32_t*)gc_alloc_old(sizeof(int32_t) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nb);
    unsigned char* nt = (unsigned char*)gc_alloc_old(sizeof(unsigned char) * (size_t)new_cap, VAL_TYPED_MAP);
    gc_mark_internal_buf(nt);

    /* 未占用槽位置零：避免 GC 保守扫描读到残留指针 */
    memset(nk, 0, keysz * (size_t)new_cap);
    memset(nv, 0, valsz * (size_t)new_cap);
    for(int i = 0; i < new_cap; i++) {
        nb[i] = -1;
        nn[i] = -1;
        nl[i] = nr[i] = np[i] = -1;
        nt[i] = 0;
    }

    /* 第二遍：紧凑写入并重建桶链（整理后统一退化为链表） */
    for(int b = 0; b < m->cap; b++) {
        int32_t cur = m->buckets[b];
        if(cur == -1) continue;
        if(m->tree[b]) {
            int32_t stk[256];
            int top = 0;
            int32_t node = cur;
            while(node != -1 || top > 0) {
                while(node != -1) {
                    if(top < 256) stk[top++] = node;
                    node = m->left[node];
                }
                if(top == 0) break;
                node = stk[--top];
                tmap_compact_place(m, node, remap[node], nk, nv, nh, nn, nb, new_cap, keysz, valsz);
                node = m->right[node];
            }
        } else {
            while(cur != -1) {
                int32_t nx = m->next[cur];
                tmap_compact_place(m, cur, remap[cur], nk, nv, nh, nn, nb, new_cap, keysz, valsz);
                cur = nx;
            }
        }
    }

    m->keys = nk;
    m->values = nv;
    m->hashes = nh;
    m->next = nn;
    m->left = nl;
    m->right = nr;
    m->parent = np;
    m->rb_color = ncol;
    m->buckets = nb;
    m->tree = nt;
    m->cap = new_cap;
    m->free_head = -1;
    m->free_count = 0;
    free(remap);
}

/* 运行时推断：全键同类型 + 全值同类型（且与目标类型一致）时按原类型装箱；
 * 任一条件不满足返回 NULL，由调用方回退逐条强转转换。 */
TypedMap* lumyr_map_to_tmap_infer(ValueMap* vm, ValueType want_key, ValueType want_val) {
    if(!vm || vm->len <= 0) return NULL;   /* 空 map 无类型信息可推断 */

    /* 第一遍：只读 Value.type 推断类型，不分配（避免类型不匹配时的无谓分配） */
    MapIter it;
    map_iter_init(&it, vm);
    Value k, v;
    if(!map_iter_next(&it, &k, &v)) return NULL;
    ValueType kt = k.type, vt = v.type;
    if(kt == VAL_NONE || vt == VAL_NONE) return NULL;
    while(map_iter_next(&it, &k, &v)) {
        if(k.type != kt || v.type != vt) return NULL;   /* 混合类型：不推断 */
    }
    if(want_key != VAL_NONE && want_key != kt) return NULL;
    if(want_val != VAL_NONE && want_val != vt) return NULL;

    /* 第二遍：按推断出的类型直接装箱写入 */
    TypedMap* m = lumyr_tmap_new(kt, vt, vm->len);
    gc_disable();
    map_iter_init(&it, vm);
    while(map_iter_next(&it, &k, &v)) {
        lumyr_tmap_set(m, k, v);
    }
    gc_enable();
    return m;
}

/* ==================== 值相等（3.11 第 4 条） ==================== */

/* 单槽位对拍：tb 非空时与 typed map 比，否则与动态 ValueMap 比。
 * 键从 a 的类型化槽位还原为 Value 后查询——hash/相等走与 VAL_MAP 一致的
 * 跨子类型值契约，因此两侧 key_type 不同（如 int vs long）也能正确对拍。 */
static int tmap_eq_slot(TypedMap* a, int32_t slot, TypedMap* tb, struct ValueMap* mb) {
    Value k = tmap_read_value(a->key_type, a->keys, slot);
    Value va = tmap_read_value(a->value_type, a->values, slot);
    Value vb;
    if(tb) {
        vb = lumyr_tmap_get(tb, k);
        /* typed map 存储值类型必为 value_type（非 VAL_NONE），NONE 即键不存在 */
        if(vb.type == VAL_NONE) return 0;
    } else {
        Value mv;
        memset(&mv, 0, sizeof(mv));
        mv.type = VAL_MAP;
        mv.v.map = mb;
        if(!lumyr_map_has(mv, k)) return 0;
        vb = lumyr_map_get(mv, k);
    }
    return lumyr_eq(va, vb).v.b;
}

/* 按桶枚举 a 的全部存活槽位（树桶中序、链表桶走 next），逐槽位对拍 */
static int tmap_eq_walk(TypedMap* a, TypedMap* tb, struct ValueMap* mb) {
    for(int b = 0; b < a->cap; b++) {
        int32_t cur = a->buckets[b];
        if(cur == -1) continue;
        if(a->tree[b]) {
            int32_t stk[256];
            int top = 0;
            int32_t node = cur;
            while(node != -1 || top > 0) {
                while(node != -1) {
                    if(top < 256) stk[top++] = node;
                    node = a->left[node];
                }
                if(top == 0) break;
                node = stk[--top];
                if(!tmap_eq_slot(a, node, tb, mb)) return 0;
                node = a->right[node];
            }
        } else {
            while(cur != -1) {
                if(!tmap_eq_slot(a, cur, tb, mb)) return 0;
                cur = a->next[cur];
            }
        }
    }
    return 1;
}

int lumyr_tmap_eq(TypedMap* a, TypedMap* b) {
    if(a == b) return 1;
    if(!a || !b) return 0;
    if(a->len != b->len) return 0;
    return tmap_eq_walk(a, b, NULL);
}

int lumyr_tmap_eq_value_map(TypedMap* a, struct ValueMap* b) {
    if(!a || !b) return 0;
    if(a->len != (int)b->len) return 0;
    return tmap_eq_walk(a, NULL, b);
}

void lumyr_tmap_free(TypedMap* m) {
    if(!m) return;
    /* 栈分配实例由调用方释放内存；堆分配由 GC sweep 回收 */
    /* 此函数供 value_free 与 GC sweep 共用，避免双释放 */
    /* PTR 族 key/value 元素递归释放由 GC mark 链路处理，此处不手动 free */
}
