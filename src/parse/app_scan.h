/*
 * lumyr 应用配置目录扫描（编译期）
 *
 * 背景：Controller/Service 支持通过应用配置文件的 scanDirs 自动发现。
 * 该发现必须在「import 文本预处理」阶段完成——运行时没有动态加载源文件
 * 的能力。流程：
 *   1. 从主文件文本定位应用配置路径（构造器/loadConfig 的 .json 字面量）；
 *   2. 解析配置 JSON 的 scanDirs（多目录）与 autoScan；
 *   3. 非递归枚举各目录 *.lm，跨目录文件名去重；
 *   4. 经 import.c 的 expand_file 展开为 mangled 模块体（与手工 import 同等待遇）。
 *
 * 结果在进程内保留（app_scan_last），供 Task 3/4 的元信息收集与引导生成使用。
 */
#ifndef LM_APP_SCAN_H
#define LM_APP_SCAN_H

/* 扫描到的隐式模块记录 */
typedef struct {
    char* abs_path;  /* 模块文件绝对路径（malloc'd） */
    int   module_id; /* 展开时分配的模块 id（-1=未展开） */
} ScannedModule;

/* 一次扫描的完整结果 */
typedef struct {
    ScannedModule* mods;      /* 扫描模块列表 */
    int            count;
    int            cap;
    char*          config_path; /* 配置文件绝对路径（malloc'd，NULL=未找到） */
    char*          config_dir;  /* 配置文件所在目录（malloc'd） */
    int            auto_scan;   /* 1=onStart 自动 scan（默认） */
} AppScanResult;

/*
 * 执行扫描。
 *   main_text：主文件 transform 后文本（含字符串字面量）
 *   main_real：主文件绝对路径（循环检测与相对路径基准）
 *   scanned_bodies：输出，全部扫描模块展开文本拼接（malloc'd，NULL=无），调用方 free
 * 返回 AppScanResult*（无配置/无目录时 count=0，仍为非 NULL）；致命错误返回 NULL
 * （错误已双语打印）。
 */
AppScanResult* app_scan_run(const char* main_text, const char* main_real,
                            char** scanned_bodies);

/* 取本次扫描结果（进程内保留；未扫描返回 NULL） */
AppScanResult* app_scan_last(void);

/* 释放扫描结果（通常进程结束自然释放，测试场景显式调用） */
void app_scan_free(AppScanResult* r);

#endif /* LM_APP_SCAN_H */
