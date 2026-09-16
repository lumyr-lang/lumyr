// lm_map.c —— 字典（哈希表 + 红黑树自适应，Java HashMap 策略）
// 初始容量 16，负载因子 0.75；桶链表>8 且总容量>=64 → 红黑树；红黑树<6 → 退化为链表
#include "lm_map.h"
#include "lm_value.h"
#include "lm_json.h"
#include "gc_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define MAP_INIT_CAP 16
#define MAP_LOAD_FACTOR 0.75
#define MAP_TREEIFY_THRESHOLD 8
#define MAP_UNTREEIFY_THRESHOLD 6
#define MAP_MIN_TREEIFY_CAP 64
#define MAP_RED 0
#define MAP_BLACK 1

// ============ 哈希函数 ============
static uint32_t value_hash(Value v) {
    uint32_t h = (uint32_t)v.type * 2654435761u;
    switch(v.type) {
        case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
        case VAL_BYTE: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT64:
        case VAL_LONG: case VAL_ULONG: case VAL_SIZE_T: case VAL_SSIZE_T:
            h ^= (uint32_t)(lumyr_extract_ll(v) * 2654435761u);
            break;
        case VAL_DOUBLE: {
            uint64_t bits;
            memcpy(&bits, &v.v.d, sizeof(bits));
            h ^= (uint32_t)(bits ^ (bits >> 32));
            break;
        }
        case VAL_BOOL:
            h ^= v.v.b ? 1 : 0;
            break;
        case VAL_CHAR:
            h ^= (uint32_t)(unsigned char)v.v.c;
            break;
        case VAL_STRING: {
            const char* s = lumyr_str_cstr(&v) ? lumyr_str_cstr(&v) : "";
            uint32_t hh = 5381;
            while(*s) hh = ((hh << 5) + hh) + (unsigned char)*s++;
            h ^= hh;
            break;
        }
        case VAL_MAP: {
            // map 键：用 JSON 字符串的哈希（顺序无关，因为 JSON 序列化顺序固定）
            char* js = lumyr_json_stringify(v);
            if(js) {
                uint32_t th = 5381;
                for(const char* p = js; *p; p++) th = ((th << 5) + th) + (unsigned char)*p;
                h ^= th;
                free(js);
            }
            break;
        }
        default:
            break;
    }
    // 扰动函数（Java HashMap 的 hash 扰动）
    h ^= (h >> 16);
    return h;
}

// ============ 键比较（红黑树排序用，严格弱序） ============
// 返回 -1/0/1
static int key_compare(Value a, Value b) {
    if(a.type != b.type) {
        // int/byte 互通
        if((a.type == VAL_INT || a.type == VAL_INT8 || a.type == VAL_INT16 || a.type == VAL_INT32 || a.type == VAL_INT64 || a.type == VAL_BYTE || a.type == VAL_UINT8 || a.type == VAL_UINT16 || a.type == VAL_UINT32 || a.type == VAL_UINT64 || a.type == VAL_LONG || a.type == VAL_ULONG || a.type == VAL_SIZE_T || a.type == VAL_SSIZE_T) &&
           (b.type == VAL_INT || b.type == VAL_INT8 || b.type == VAL_INT16 || b.type == VAL_INT32 || b.type == VAL_INT64 || b.type == VAL_BYTE || b.type == VAL_UINT8 || b.type == VAL_UINT16 || b.type == VAL_UINT32 || b.type == VAL_UINT64 || b.type == VAL_LONG || b.type == VAL_ULONG || b.type == VAL_SIZE_T || b.type == VAL_SSIZE_T))
            return (lumyr_extract_ll(a) > lumyr_extract_ll(b)) - (lumyr_extract_ll(a) < lumyr_extract_ll(b));
        return (a.type < b.type) ? -1 : 1;
    }
    switch(a.type) {
        case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
        case VAL_BYTE: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT64:
        case VAL_LONG: case VAL_ULONG: case VAL_SIZE_T: case VAL_SSIZE_T:
            return (lumyr_extract_ll(a) > lumyr_extract_ll(b)) - (lumyr_extract_ll(a) < lumyr_extract_ll(b));
        case VAL_DOUBLE:
            return (a.v.d > b.v.d) - (a.v.d < b.v.d);
        case VAL_BOOL:
            return (a.v.b > b.v.b) - (a.v.b < b.v.b);
        case VAL_CHAR:
            return ((unsigned char)a.v.c > (unsigned char)b.v.c) -
                   ((unsigned char)a.v.c < (unsigned char)b.v.c);
        case VAL_STRING: {
            int c = strcmp(lumyr_str_cstr(&a) ? lumyr_str_cstr(&a) : "", lumyr_str_cstr(&b) ? lumyr_str_cstr(&b) : "");
            return (c > 0) - (c < 0);
        }
        case VAL_MAP: {
            char* sa = lumyr_json_stringify(a);
            char* sb = lumyr_json_stringify(b);
            int c = strcmp(sa ? sa : "", sb ? sb : "");
            free(sa); free(sb);
            return (c > 0) - (c < 0);
        }
        default:
            return 0;
    }
}

// 键相等（先比 hash 再比值）
static int key_eq(Value a, Value b) {
    if(a.type != b.type) {
        if((a.type == VAL_INT || a.type == VAL_INT8 || a.type == VAL_INT16 || a.type == VAL_INT32 || a.type == VAL_INT64 || a.type == VAL_BYTE || a.type == VAL_UINT8 || a.type == VAL_UINT16 || a.type == VAL_UINT32 || a.type == VAL_UINT64 || a.type == VAL_LONG || a.type == VAL_ULONG || a.type == VAL_SIZE_T || a.type == VAL_SSIZE_T) &&
           (b.type == VAL_INT || b.type == VAL_INT8 || b.type == VAL_INT16 || b.type == VAL_INT32 || b.type == VAL_INT64 || b.type == VAL_BYTE || b.type == VAL_UINT8 || b.type == VAL_UINT16 || b.type == VAL_UINT32 || b.type == VAL_UINT64 || b.type == VAL_LONG || b.type == VAL_ULONG || b.type == VAL_SIZE_T || b.type == VAL_SSIZE_T))
            return lumyr_extract_ll(a) == lumyr_extract_ll(b);
        return 0;
    }
    switch(a.type) {
        case VAL_INT: case VAL_INT8: case VAL_INT16: case VAL_INT32: case VAL_INT64:
        case VAL_BYTE: case VAL_UINT8: case VAL_UINT16: case VAL_UINT32: case VAL_UINT64:
        case VAL_LONG: case VAL_ULONG: case VAL_SIZE_T: case VAL_SSIZE_T:
            return lumyr_extract_ll(a) == lumyr_extract_ll(b);
        case VAL_DOUBLE: return a.v.d == b.v.d;
        case VAL_BOOL: return a.v.b == b.v.b;
        case VAL_CHAR: return a.v.c == b.v.c;
        case VAL_STRING: return strcmp(lumyr_str_cstr(&a) ? lumyr_str_cstr(&a) : "", lumyr_str_cstr(&b) ? lumyr_str_cstr(&b) : "") == 0;
        case VAL_MAP: {
            if(a.v.map->len != b.v.map->len) return 0;
            MapIter it; map_iter_init(&it, a.v.map);
            Value k, vv;
            while(map_iter_next(&it, &k, &vv)) {
                if(!lumyr_map_has((Value){.type=VAL_MAP,.v.map=b.v.map}, k)) return 0;
                Value bv = lumyr_map_get((Value){.type=VAL_MAP,.v.map=b.v.map}, k);
                if(!key_eq(vv, bv)) return 0;
            }
            return 1;
        }
        default: return 0;
    }
}

// ============ Entry 管理 ============
static MapEntry* entry_new(Value key, Value val, uint32_t hash) {
    MapEntry* e = (MapEntry*)gc_alloc_old(sizeof(MapEntry), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(e);  /* 标记为内部缓冲区，保守 C 栈扫描跳过（与 buckets/tree 一致） */
    gc_write_barrier(key);  /* 增量标记写屏障 */
    e->key = key;
    gc_write_barrier(val);  /* 增量标记写屏障 */
    e->value = val;
    e->hash = hash;
    e->color = MAP_RED;
    return e;
}

// entry_free：引用语义 + GC，空操作（entry 节点由 GC 统一回收）
void entry_free(MapEntry* e) {
    (void)e;
}

// ============ 桶索引 ============
static inline int bucket_idx(uint32_t hash, int cap) {
    return hash & (cap - 1);  // cap 是 2 的幂
}

// ============ 链表操作 ============
static MapEntry* list_find(MapEntry* head, Value key, uint32_t hash) {
    MapEntry* e = head;
    while(e) {
        if(e->hash == hash && key_eq(e->key, key)) return e;
        e = e->next;
    }
    return NULL;
}

static int list_count(MapEntry* head) {
    int n = 0;
    while(head) { n++; head = head->next; }
    return n;
}

// ============ 红黑树操作 ============
static MapEntry* tree_find(MapEntry* root, Value key, uint32_t hash) {
    MapEntry* e = root;
    while(e) {
        int cmp;
        if(e->hash != hash) cmp = (e->hash > hash) ? 1 : -1;
        else cmp = key_compare(e->key, key);
        if(cmp == 0) {
            if(key_eq(e->key, key)) return e;
            e = e->right;  // hash 相同但 key 不同，继续右子树找
        } else if(cmp < 0) e = e->right;
        else e = e->left;
    }
    return NULL;
}

// 红黑树左旋
static void rotate_left(ValueMap* m, int idx, MapEntry* x) {
    MapEntry* y = x->right;
    x->right = y->left;
    if(y->left) y->left->parent = x;
    y->parent = x->parent;
    if(!x->parent) m->buckets[idx] = y;
    else if(x == x->parent->left) x->parent->left = y;
    else x->parent->right = y;
    y->left = x;
    x->parent = y;
}

// 红黑树右旋
static void rotate_right(ValueMap* m, int idx, MapEntry* x) {
    MapEntry* y = x->left;
    x->left = y->right;
    if(y->right) y->right->parent = x;
    y->parent = x->parent;
    if(!x->parent) m->buckets[idx] = y;
    else if(x == x->parent->right) x->parent->right = y;
    else x->parent->left = y;
    y->right = x;
    x->parent = y;
}

// 红黑树插入后平衡
static void tree_insert_fixup(ValueMap* m, int idx, MapEntry* z) {
    while(z->parent && z->parent->color == MAP_RED) {
        if(z->parent == z->parent->parent->left) {
            MapEntry* y = z->parent->parent->right;
            if(y && y->color == MAP_RED) {
                z->parent->color = MAP_BLACK;
                y->color = MAP_BLACK;
                z->parent->parent->color = MAP_RED;
                z = z->parent->parent;
            } else {
                if(z == z->parent->right) { z = z->parent; rotate_left(m, idx, z); }
                z->parent->color = MAP_BLACK;
                z->parent->parent->color = MAP_RED;
                rotate_right(m, idx, z->parent->parent);
            }
        } else {
            MapEntry* y = z->parent->parent->left;
            if(y && y->color == MAP_RED) {
                z->parent->color = MAP_BLACK;
                y->color = MAP_BLACK;
                z->parent->parent->color = MAP_RED;
                z = z->parent->parent;
            } else {
                if(z == z->parent->left) { z = z->parent; rotate_right(m, idx, z); }
                z->parent->color = MAP_BLACK;
                z->parent->parent->color = MAP_RED;
                rotate_left(m, idx, z->parent->parent);
            }
        }
    }
    m->buckets[idx]->color = MAP_BLACK;
}

// 红黑树插入
static void tree_insert(ValueMap* m, int idx, MapEntry* z) {
    MapEntry* y = NULL;
    MapEntry* x = m->buckets[idx];
    while(x) {
        y = x;
        int cmp;
        if(z->hash != x->hash) cmp = (z->hash > x->hash) ? 1 : -1;
        else cmp = key_compare(z->key, x->key);
        if(cmp < 0) x = x->left;
        else if(cmp > 0) x = x->right;
        else { x = x->right; }  // 重复键不会走到这里（set 前已检查）
    }
    z->parent = y;
    if(!y) m->buckets[idx] = z;
    else if(key_compare(z->key, y->key) < 0) y->left = z;
    else y->right = z;
    z->color = MAP_RED;
    z->left = z->right = NULL;
    tree_insert_fixup(m, idx, z);
}

// 红黑树最小节点
static MapEntry* tree_min(MapEntry* x) {
    while(x->left) x = x->left;
    return x;
}

// 红黑树删除后平衡
static void tree_delete_fixup(ValueMap* m, int idx, MapEntry* x, MapEntry* x_parent) {
    while(x != m->buckets[idx] && (!x || x->color == MAP_BLACK)) {
        if(!x_parent) break;
        if(x == x_parent->left) {
            MapEntry* w = x_parent->right;
            if(w && w->color == MAP_RED) {
                w->color = MAP_BLACK; x_parent->color = MAP_RED;
                rotate_left(m, idx, x_parent); w = x_parent->right;
            }
            if((!w->left || w->left->color == MAP_BLACK) &&
               (!w->right || w->right->color == MAP_BLACK)) {
                if(w) w->color = MAP_RED;
                x = x_parent; x_parent = x->parent;
            } else {
                if(!w->right || w->right->color == MAP_BLACK) {
                    if(w->left) w->left->color = MAP_BLACK;
                    if(w) w->color = MAP_RED;
                    rotate_right(m, idx, w); w = x_parent->right;
                }
                if(w) w->color = x_parent->color;
                x_parent->color = MAP_BLACK;
                if(w && w->right) w->right->color = MAP_BLACK;
                rotate_left(m, idx, x_parent);
                x = m->buckets[idx];
                break;
            }
        } else {
            MapEntry* w = x_parent->left;
            if(w && w->color == MAP_RED) {
                w->color = MAP_BLACK; x_parent->color = MAP_RED;
                rotate_right(m, idx, x_parent); w = x_parent->left;
            }
            if((!w->right || w->right->color == MAP_BLACK) &&
               (!w->left || w->left->color == MAP_BLACK)) {
                if(w) w->color = MAP_RED;
                x = x_parent; x_parent = x->parent;
            } else {
                if(!w->left || w->left->color == MAP_BLACK) {
                    if(w->right) w->right->color = MAP_BLACK;
                    if(w) w->color = MAP_RED;
                    rotate_left(m, idx, w); w = x_parent->left;
                }
                if(w) w->color = x_parent->color;
                x_parent->color = MAP_BLACK;
                if(w && w->left) w->left->color = MAP_BLACK;
                rotate_right(m, idx, x_parent);
                x = m->buckets[idx];
                break;
            }
        }
    }
    if(x) x->color = MAP_BLACK;
}

// 红黑树删除节点
static void tree_remove(ValueMap* m, int idx, MapEntry* z) {
    MapEntry* y = z;
    MapEntry* x;
    MapEntry* x_parent;
    int y_original_color = y->color;
    if(!z->left) {
        x = z->right;
        x_parent = z->parent;
        if(!z->parent) m->buckets[idx] = z->right;
        else if(z == z->parent->left) z->parent->left = z->right;
        else z->parent->right = z->right;
        if(z->right) z->right->parent = z->parent;
    } else if(!z->right) {
        x = z->left;
        x_parent = z->parent;
        if(!z->parent) m->buckets[idx] = z->left;
        else if(z == z->parent->left) z->parent->left = z->left;
        else z->parent->right = z->left;
        if(z->left) z->left->parent = z->parent;
    } else {
        y = tree_min(z->right);
        y_original_color = y->color;
        x = y->right;
        if(y->parent == z) {
            x_parent = y;
        } else {
            x_parent = y->parent;
            if(y->right) y->right->parent = y->parent;
            y->parent->left = y->right;
            y->right = z->right;
            y->right->parent = y;
        }
        if(!z->parent) m->buckets[idx] = y;
        else if(z == z->parent->left) z->parent->left = y;
        else z->parent->right = y;
        y->parent = z->parent;
        y->color = z->color;
        y->left = z->left;
        y->left->parent = y;
    }
    if(y_original_color == MAP_BLACK)
        tree_delete_fixup(m, idx, x, x_parent);
    entry_free(z);
}

// 红黑树节点数
static int tree_count(MapEntry* root) {
    if(!root) return 0;
    return 1 + tree_count(root->left) + tree_count(root->right);
}

// ============ 链表 ↔ 红黑树转换 ============
static void treeify_bin(ValueMap* m, int idx) {
    MapEntry* head = m->buckets[idx];
    if(!head || m->cap < MAP_MIN_TREEIFY_CAP) return;
    // 把链表转成红黑树
    MapEntry* root = NULL;
    MapEntry* e = head;
    while(e) {
        MapEntry* next = e->next;
        e->left = e->right = e->parent = NULL;
        e->next = NULL;
        // 插入到红黑树
        MapEntry* y = NULL;
        MapEntry* x = root;
        while(x) {
            y = x;
            int cmp;
            if(e->hash != x->hash) cmp = (e->hash > x->hash) ? 1 : -1;
            else cmp = key_compare(e->key, x->key);
            if(cmp < 0) x = x->left;
            else x = x->right;
        }
        e->parent = y;
        if(!y) root = e;
        else if(key_compare(e->key, y->key) < 0) y->left = e;
        else y->right = e;
        e->color = MAP_RED;
        // 插入平衡（简化版，直接用 tree_insert_fixup 需要 root 在 m->buckets[idx]）
        m->buckets[idx] = root;
        tree_insert_fixup(m, idx, e);
        root = m->buckets[idx];
        e = next;
    }
    m->tree[idx] = 1;
}

static void untreeify_bin(ValueMap* m, int idx) {
    // 红黑树中序遍历转链表
    MapEntry* root = m->buckets[idx];
    MapEntry* head = NULL;
    MapEntry* tail = NULL;
    // 非递归中序遍历
    MapEntry* stack[128];
    int top = 0;
    MapEntry* cur = root;
    while(cur || top > 0) {
        while(cur) { stack[top++] = cur; cur = cur->left; }
        cur = stack[--top];
        MapEntry* next = cur->right;
        cur->left = cur->right = cur->parent = NULL;
        cur->next = NULL;
        if(!head) head = cur;
        else tail->next = cur;
        tail = cur;
        cur = next;
    }
    m->buckets[idx] = head;
    m->tree[idx] = 0;
}

// ============ 扩容 rehash ============
static void map_resize(ValueMap* m) {
    int old_cap = m->cap;
    int new_cap = old_cap * 2;
    MapEntry** new_buckets = (MapEntry**)gc_alloc_old(new_cap * sizeof(MapEntry*), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(new_buckets);  /* 标记为内部缓冲区，保守 C 栈扫描跳过 */
    unsigned char* new_tree = (unsigned char*)gc_alloc_old(new_cap * sizeof(unsigned char), VAL_MAP);  /* 内部缓冲区老年代 */
    gc_mark_internal_buf(new_tree);
    for(int i = 0; i < old_cap; i++) {
        MapEntry* e = m->buckets[i];
        if(!e) continue;
        if(m->tree[i]) {
            // 红黑树：中序遍历，每个节点重新分配
            MapEntry* stack[128];
            int top = 0;
            MapEntry* cur = e;
            while(cur || top > 0) {
                while(cur) { stack[top++] = cur; cur = cur->left; }
                cur = stack[--top];
                MapEntry* next = cur->right;
                int ni = bucket_idx(cur->hash, new_cap);
                cur->left = cur->right = cur->parent = NULL;
                cur->next = new_buckets[ni];
                new_buckets[ni] = cur;
                cur = next;
            }
        } else {
            // 链表：拆分到两个桶
            MapEntry* lo_head = NULL, *lo_tail = NULL;
            MapEntry* hi_head = NULL, *hi_tail = NULL;
            while(e) {
                MapEntry* next = e->next;
                e->next = NULL;
                if((e->hash & old_cap) == 0) {
                    if(!lo_head) lo_head = e; else lo_tail->next = e;
                    lo_tail = e;
                } else {
                    if(!hi_head) hi_head = e; else hi_tail->next = e;
                    hi_tail = e;
                }
                e = next;
            }
            if(lo_head) new_buckets[i] = lo_head;
            if(hi_head) new_buckets[i + old_cap] = hi_head;
        }
    }
    m->buckets = new_buckets;
    m->tree = new_tree;
    m->cap = new_cap;
    // 重新检查是否需要 treeify（扩容后链表可能变短，不需要立即 treeify）
}

// ============ 公共 API ============
int lumyr_map_find(const ValueMap* m, Value key) {
    uint32_t h = value_hash(key);
    int idx = bucket_idx(h, m->cap);
    if(m->tree[idx]) {
        return tree_find(m->buckets[idx], key, h) ? 0 : -1;
    } else {
        return list_find(m->buckets[idx], key, h) ? 0 : -1;
    }
}

void lumyr_map_set(Value* map, Value key, Value val) {
    if(map->type != VAL_MAP) runtime_error("字典下标写需要 字典[键]");
    /* Remembered set 检查：老年代 map 写入新生代 key/val 时加入 rs */
    gc_remembered_set_check(*map, key);
    gc_remembered_set_check(*map, val);
    ValueMap* m = map->v.map;
    uint32_t h = value_hash(key);
    int idx = bucket_idx(h, m->cap);
    if(m->tree[idx]) {
        MapEntry* e = tree_find(m->buckets[idx], key, h);
        if(e) { gc_write_barrier(val); e->value = val; return; }
        MapEntry* ne = entry_new(key, val, h);
        tree_insert(m, idx, ne);
    } else {
        MapEntry* e = list_find(m->buckets[idx], key, h);
        if(e) { gc_write_barrier(val); e->value = val; return; }
        MapEntry* ne = entry_new(key, val, h);
        ne->next = m->buckets[idx];
        m->buckets[idx] = ne;
        // 检查是否需要 treeify
        if(list_count(m->buckets[idx]) >= MAP_TREEIFY_THRESHOLD)
            treeify_bin(m, idx);
    }
    m->len++;
    // 检查是否需要扩容
    if(m->len > (int)(m->cap * MAP_LOAD_FACTOR))
        map_resize(m);
}

Value lumyr_map_get(Value map, Value key) {
    if(map.type != VAL_MAP) runtime_error("字典下标读需要 字典[键]");
    ValueMap* m = map.v.map;
    uint32_t h = value_hash(key);
    int idx = bucket_idx(h, m->cap);
    MapEntry* e;
    if(m->tree[idx]) e = tree_find(m->buckets[idx], key, h);
    else e = list_find(m->buckets[idx], key, h);
    if(!e) return val_none();
    return e->value;
}

int lumyr_map_has(Value map, Value key) {
    if(map.type != VAL_MAP) return 0;
    return lumyr_map_find(map.v.map, key) >= 0;
}

Value lumyr_map_del(Value* map, Value key) {
    if(map->type != VAL_MAP) runtime_error("del() 参数必须是数组或字典");
    ValueMap* m = map->v.map;
    uint32_t h = value_hash(key);
    int idx = h & (m->cap - 1);
    if(m->tree[idx]) {
        // 红黑树查找并删除
        MapEntry* cur = m->buckets[idx];
        while(cur) {
            int cmp = key_compare(cur->key, key);
            if(cmp == 0) {
                tree_remove(m, idx, cur);
                m->len--;
                // 红黑树节点数 < 阈值 → 退化为链表
                if(tree_count(m->buckets[idx]) < MAP_UNTREEIFY_THRESHOLD)
                    untreeify_bin(m, idx);
                return *map;
            }
            cur = (cmp < 0) ? cur->right : cur->left;
        }
    } else {
        // 链表查找并删除
        MapEntry* prev = NULL;
        MapEntry* cur = m->buckets[idx];
        while(cur) {
            if(key_eq(cur->key, key)) {
                if(prev) prev->next = cur->next;
                else m->buckets[idx] = cur->next;
                entry_free(cur);
                m->len--;
                return *map;
            }
            prev = cur;
            cur = cur->next;
        }
    }
    return *map;  // 键不存在，无操作
}

Value lumyr_map_keys(Value map) {
    if(map.type != VAL_MAP) runtime_error("keys() 参数必须是字典");
    ValueMap* m = map.v.map;
    Value r = val_array(m->len);
    int pos = 0;
    for(int i = 0; i < m->cap && pos < m->len; i++) {
        MapEntry* e = m->buckets[i];
        if(m->tree[i]) {
            MapEntry* stack[128];
            int top = 0;
            MapEntry* cur = e;
            while(cur || top > 0) {
                while(cur) { stack[top++] = cur; cur = cur->left; }
                cur = stack[--top];
                gc_write_barrier(cur->key);  /* 增量标记写屏障 */
                r.v.array->items[pos++] = cur->key;
                cur = cur->right;
            }
        } else {
            while(e) {
                gc_write_barrier(e->key);  /* 增量标记写屏障 */
                r.v.array->items[pos++] = e->key;
                e = e->next;
            }
        }
    }
    return r;
}

Value lumyr_map_values(Value map) {
    if(map.type != VAL_MAP) runtime_error("values() 参数必须是字典");
    ValueMap* m = map.v.map;
    Value r = val_array(m->len);
    int pos = 0;
    for(int i = 0; i < m->cap && pos < m->len; i++) {
        MapEntry* e = m->buckets[i];
        if(m->tree[i]) {
            MapEntry* stack[128];
            int top = 0;
            MapEntry* cur = e;
            while(cur || top > 0) {
                while(cur) { stack[top++] = cur; cur = cur->left; }
                cur = stack[--top];
                gc_write_barrier(cur->value);  /* 增量标记写屏障 */
                r.v.array->items[pos++] = cur->value;
                cur = cur->right;
            }
        } else {
            while(e) {
                gc_write_barrier(e->value);  /* 增量标记写屏障 */
                r.v.array->items[pos++] = e->value;
                e = e->next;
            }
        }
    }
    return r;
}

Value lumyr_map_lit(Value* kv, int n) {
    Value r = val_map();
    for(int i = 0; i < n; i++)
        lumyr_map_set(&r, kv[i * 2], kv[i * 2 + 1]);
    return r;
}

// ============ 迭代器实现 ============
void map_iter_init(MapIter* it, ValueMap* m) {
    it->map = m;
    it->bucket_idx = -1;
    it->entry = NULL;
    it->in_tree = 0;
    it->tree_top = 0;
}

int map_iter_next(MapIter* it, Value* key, Value* val) {
    ValueMap* m = it->map;
    // 如果当前在红黑树中序遍历
    if(it->in_tree && it->tree_top > 0) {
        MapEntry* cur = it->tree_stack[--it->tree_top];
        if(key) *key = cur->key;
        if(val) *val = cur->value;
        if(cur->right) {
            MapEntry* n = cur->right;
            while(n) { it->tree_stack[it->tree_top++] = n; n = n->left; }
        }
        return 1;
    }
    // 如果当前在链表中
    if(it->entry) {
        it->entry = it->entry->next;
        if(it->entry) {
            if(key) *key = it->entry->key;
            if(val) *val = it->entry->value;
            return 1;
        }
    }
    // 找下一个非空桶
    it->bucket_idx++;
    while(it->bucket_idx < m->cap) {
        if(m->buckets[it->bucket_idx]) {
            if(m->tree[it->bucket_idx]) {
                // 红黑树：中序遍历入栈
                it->in_tree = 1;
                it->tree_top = 0;
                MapEntry* cur = m->buckets[it->bucket_idx];
                while(cur) { it->tree_stack[it->tree_top++] = cur; cur = cur->left; }
                if(it->tree_top > 0) {
                    MapEntry* node = it->tree_stack[--it->tree_top];
                    if(key) *key = node->key;
                    if(val) *val = node->value;
                    if(node->right) {
                        MapEntry* n = node->right;
                        while(n) { it->tree_stack[it->tree_top++] = n; n = n->left; }
                    }
                    return 1;
                }
            } else {
                // 链表
                it->in_tree = 0;
                it->entry = m->buckets[it->bucket_idx];
                if(key) *key = it->entry->key;
                if(val) *val = it->entry->value;
                return 1;
            }
        }
        it->bucket_idx++;
    }
    return 0;
}


// 浅拷贝 map：嵌套 struct 递归浅拷贝（C 语义：memcpy 嵌套 struct 也是值拷贝）
Value lumyr_map_shallow_copy(Value map) {
    if(map.type != VAL_MAP) return map;
    ValueMap* m = map.v.map;
    Value r = val_map();
    MapIter it;
    map_iter_init(&it, m);
    Value k, v;
    while(map_iter_next(&it, &k, &v)) {
        /* 嵌套 struct：值是 map 且包含 __structname__ 键，递归浅拷贝 */
        if(v.type == VAL_MAP && lumyr_map_has(v, lumyr_make_string("__structname__"))) {
            v = lumyr_map_shallow_copy(v);
        }
        lumyr_map_set(&r, k, v);
    }
    return r;
}
