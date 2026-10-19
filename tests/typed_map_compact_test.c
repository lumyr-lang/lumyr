// typed_map_compact_test.c —— TypedMap 删除策略单测（对齐调研 3.10 free_list + 主动 compact 与阶段 4 验收）
// 直接 C 层调用，可观测内部 cap / free_count / free_head，验证阈值精确触发与缩容。
// 覆盖：
//   1) free_list 槽位复用：删除维护空闲链，新增优先复用，不扩容；
//   2) 60% 阈值精确触发：free_count*100 <= cap*60 不 compact，越过即 compact 并缩容；
//   3) 高频增删内存归还：大 cap 经 compact 大幅缩容，存活键完整可查；
//   4) lumyr_map_to_tmap_infer 契约：均匀类型装箱 / 类型不匹配或空 map 返回 NULL。
#include "lumyr_typed_maps.h"
#include "lumyr_value.h"
#include "lm_map.h"
#include <stdio.h>
#include <string.h>

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  FAIL: %s (line %d)\n", msg, __LINE__); g_fail = 1; } \
} while (0)

/* 构造字符串键 "k<i>"；val_string 对短串走 SSO 内联，返回值自包含 */
static Value mkkey(int i) {
    char buf[32];
    snprintf(buf, sizeof buf, "k%d", i);
    return val_string(buf);
}

/* 插入 [start, start+count) 个键，值为键序号 */
static void fill(TypedMap* m, int start, int count) {
    for (int i = start; i < start + count; i++)
        lumyr_tmap_set(m, mkkey(i), val_int(i));
}

/* [lo, hi) 区间键均存在且值等于键序号 */
static int live_ok(TypedMap* m, int lo, int hi) {
    for (int i = lo; i < hi; i++) {
        Value v = lumyr_tmap_get(m, mkkey(i));
        if (v.type != VAL_INT || v.v.i != i) return 0;
    }
    return 1;
}

/* [lo, hi) 区间键均不存在 */
static int dead_ok(TypedMap* m, int lo, int hi) {
    for (int i = lo; i < hi; i++) {
        if (lumyr_tmap_has(m, mkkey(i))) return 0;
    }
    return 1;
}

/* ---------- 1. free_list 槽位复用（不触发 compact） ---------- */
static void test_free_list_reuse(void) {
    printf("[1] free_list 槽位复用\n");
    TypedMap* m = lumyr_tmap_new(VAL_STRING, VAL_INT, 0);
    fill(m, 0, 150);                       /* len=150 -> 第 97 次扩容到 cap=256 */
    CHECK(m->cap == 256, "cap 应为 256");
    CHECK(m->len == 150, "len 应为 150");
    CHECK(m->free_count == 0, "初始无空闲槽位");

    for (int i = 0; i < 10; i++) lumyr_tmap_del(m, mkkey(i));   /* 10/256=3.9% < 60%：不 compact */
    CHECK(m->cap == 256, "空闲占比不足不应 compact（cap 保持 256）");
    CHECK(m->free_count == 10, "free_count 应为 10");
    CHECK(m->free_head != -1, "free_head 应指向空闲链");
    CHECK(m->len == 140, "len 应为 140");

    fill(m, 1000, 10);                     /* 新增 10 个：应复用空闲槽位，不扩容 */
    CHECK(m->cap == 256, "复用空闲槽位不应扩容");
    CHECK(m->free_count == 0, "空闲槽位应被复用完");
    CHECK(m->free_head == -1, "free_head 应为空");
    CHECK(m->len == 150, "len 应回到 150");
    CHECK(live_ok(m, 10, 150), "k10..k149 应完整");
    CHECK(live_ok(m, 1000, 1010), "k1000..k1009 应完整");
}

/* ---------- 2. 60% 阈值精确触发 ---------- */
static void test_threshold(void) {
    printf("[2] 60%% 阈值精确触发 + 缩容\n");
    TypedMap* m = lumyr_tmap_new(VAL_STRING, VAL_INT, 0);
    fill(m, 0, 192);                       /* len=192 -> cap=256（192 <= 256*0.75） */
    CHECK(m->cap == 256, "cap 应为 256");
    CHECK(m->len == 192, "len 应为 192");

    for (int i = 0; i < 153; i++) lumyr_tmap_del(m, mkkey(i));  /* free=153：15300 <= 15360：不 compact */
    CHECK(m->cap == 256, "153/256=59.8% 不应 compact");
    CHECK(m->free_count == 153, "free_count 应为 153");

    lumyr_tmap_del(m, mkkey(153));         /* free=154：15400 > 15360 -> compact */
    CHECK(m->free_count == 0, "compact 后 free_count 应清零");
    CHECK(m->free_head == -1, "compact 后 free_head 应为空");
    CHECK(m->len == 38, "len 应为 38");
    CHECK(m->cap == 64, "compact 应缩容到 64（适配 38 存活）");
    CHECK(live_ok(m, 154, 192), "k154..k191 应完整");
    CHECK(dead_ok(m, 0, 154), "k0..k153 应已删除");
}

/* ---------- 3. 高频增删内存归还 ---------- */
static void test_memory_reclaim(void) {
    printf("[3] 高频增删内存归还\n");
    TypedMap* m = lumyr_tmap_new(VAL_STRING, VAL_INT, 0);
    fill(m, 0, 6000);                       /* len=6000 -> cap=8192 */
    CHECK(m->cap == 8192, "cap 应为 8192");
    int cap_before = m->cap;

    /* 删到 free=4916：491600 > 8192*60=491520 -> compact（live=1084 -> 缩容 2048） */
    int deleted = 0;
    while (deleted <= 4915) { lumyr_tmap_del(m, mkkey(deleted)); deleted++; }
    CHECK(m->cap == 2048, "compact 应缩容 8192 -> 2048");
    CHECK(m->cap < cap_before, "内存应归还（cap 下降）");
    CHECK(m->free_count == 0, "compact 后 free_count 应清零");
    CHECK(m->len == 1084, "len 应为 1084");
    CHECK(live_ok(m, 4916, 6000), "k4916..k5999 应完整");
    CHECK(dead_ok(m, 0, 4916), "k0..k4915 应已删除");

    /* 继续删除：数据仍完整（本段空闲未再越过阈值，cap 不再变） */
    while (deleted < 5900) { lumyr_tmap_del(m, mkkey(deleted)); deleted++; }
    CHECK(m->len == 100, "len 应为 100");
    CHECK(live_ok(m, 5900, 6000), "k5900..k5999 应完整");
}

/* ---------- 4. lumyr_map_to_tmap_infer 契约 ---------- */
static void test_infer(void) {
    printf("[4] lumyr_map_to_tmap_infer 契约\n");
    /* 均匀类型 string -> int：推断成功 */
    Value vm = val_map();
    lumyr_map_set(&vm, val_string("a"), val_int(11));
    lumyr_map_set(&vm, val_string("b"), val_int(22));
    TypedMap* t = lumyr_map_to_tmap_infer(vm.v.map, VAL_STRING, VAL_INT);
    CHECK(t != NULL, "均匀 map 应推断成功");
    if (t) {
        Value a = lumyr_tmap_get(t, val_string("a"));
        CHECK(a.type == VAL_INT && a.v.i == 11, "推断后 a=11");
        CHECK(t->key_type == VAL_STRING && t->value_type == VAL_INT, "类型应为 string/int");
    }
    /* 目标值型不符 -> NULL（由调用方回退逐条强转） */
    CHECK(lumyr_map_to_tmap_infer(vm.v.map, VAL_STRING, VAL_BYTE) == NULL,
          "want_val 与推断不符应返回 NULL");
    /* 空 map 无类型信息 -> NULL */
    Value empty = val_map();
    CHECK(lumyr_map_to_tmap_infer(empty.v.map, VAL_STRING, VAL_INT) == NULL,
          "空 map 应返回 NULL");
    /* 值类型混合 -> NULL */
    Value mixed = val_map();
    lumyr_map_set(&mixed, val_string("a"), val_int(1));
    lumyr_map_set(&mixed, val_string("b"), val_string("x"));
    CHECK(lumyr_map_to_tmap_infer(mixed.v.map, VAL_NONE, VAL_NONE) == NULL,
          "混合类型应返回 NULL");
    /* VAL_NONE 不限型：均匀类型仍可推断 */
    TypedMap* t3 = lumyr_map_to_tmap_infer(vm.v.map, VAL_NONE, VAL_NONE);
    CHECK(t3 != NULL && t3->key_type == VAL_STRING && t3->value_type == VAL_INT,
          "VAL_NONE 不限型应推断成功");
}

int main(void) {
    printf("=== typed_map compact/infer 单测 ===\n");
    test_free_list_reuse();
    test_threshold();
    test_memory_reclaim();
    test_infer();
    if (g_fail) { printf("=== FAILED ===\n"); return 1; }
    printf("=== ALL PASS / 全部通过 ===\n");
    return 0;
}
