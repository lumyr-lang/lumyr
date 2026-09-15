/*
 * lumyr_log.h - 统一日志和异常封装
 * 支持打印文件名、行号、错误信息主体和调用链
 */
#ifndef LUMYR_LOG_H
#define LUMYR_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/* 日志级别 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} LogLevel;

/* 调用栈最大深度 */
#define LOG_CALLSTACK_MAX_DEPTH 64

/* 调用栈条目 */
typedef struct {
    const char* func_name;      /* 函数名 */
    const char* file;           /* 文件名 */
    int line;                   /* 行号 */
    char args[512];             /* 参数信息 */
} LogCallStackEntry;

/* 全局日志级别（默认 INFO） */
extern LogLevel g_log_level;

/* 全局调用栈 */
extern LogCallStackEntry g_log_callstack[LOG_CALLSTACK_MAX_DEPTH];
extern int g_log_callstack_depth;

/* 设置日志级别 */
void log_set_level(LogLevel level);

/* 获取日志级别字符串 */
const char* log_level_str(LogLevel level);

/* 统一日志打印函数（内部使用，建议用宏） */
void log_print_impl(LogLevel level, const char* file, int line, const char* fmt, ...);

/* 统一异常抛出函数（内部使用，建议用宏） */
void log_throw_impl(const char* file, int line, const char* fmt, ...);

/* 调用栈管理函数 */
void log_push_call_impl(const char* func_name, const char* file, int line, const char* args_fmt, ...);
void log_pop_call_impl(void);

/* 打印调用栈 */
void log_print_callstack(void);

/* 宏定义：自动传入文件名和行号 */
#define LOG_DEBUG(...) log_print_impl(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_print_impl(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_print_impl(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_print_impl(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) log_print_impl(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/* 异常抛出宏：自动传入文件名和行号，打印后退出 */
#define THROW_ERROR(...) log_throw_impl(__FILE__, __LINE__, __VA_ARGS__)

/* 调用栈宏：自动传入函数名、文件名和行号 */
#define LOG_PUSH_CALL(...) log_push_call_impl(__func__, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_POP_CALL() log_pop_call_impl()

#endif /* LUMYR_LOG_H */
