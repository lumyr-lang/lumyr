/*
 * lumyr_debug.h - 调试日志模块
 * 用宏定义控制开关，调试时打开，发布时关闭
 */
#ifndef LUMYR_DEBUG_H
#define LUMYR_DEBUG_H

#include <stdio.h>

/* 调试开关：1=打开，0=关闭 */
#define LUMYR_DEBUG 1

/* 调试日志输出到文件 */
#define LUMYR_DEBUG_FILE "lumyr_debug.log"

#if LUMYR_DEBUG
    #define LUMYR_DBG(fmt, ...) do { \
        FILE* __f = fopen(LUMYR_DEBUG_FILE, "a"); \
        if(__f) { \
            fprintf(__f, "[%s:%d] " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__); \
            fclose(__f); \
        } \
    } while(0)
#else
    #define LUMYR_DBG(fmt, ...) ((void)0)
#endif

#endif /* LUMYR_DEBUG_H */
