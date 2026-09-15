/*
 * lumyr_log.h - 统一日志和异常封装
 * 支持打印文件名、行号和错误信息主体
 */
#ifndef LUMYR_LOG_H
#define LUMYR_LOG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 日志级别 */
typedef enum {
    LOG_DEBUG = 0,
    LOG_INFO = 1,
    LOG_WARN = 2,
    LOG_ERROR = 3,
    LOG_FATAL = 4
} LogLevel;

/* 全局日志级别（默认 INFO） */
extern LogLevel g_log_level;

/* 设置日志级别 */
void log_set_level(LogLevel level);

/* 获取日志级别字符串 */
const char* log_level_str(LogLevel level);

/* 统一日志打印函数（内部使用，建议用宏） */
void log_print_impl(LogLevel level, const char* file, int line, const char* fmt, ...);

/* 统一异常抛出函数（内部使用，建议用宏） */
void log_throw_impl(const char* file, int line, const char* fmt, ...);

/* 宏定义：自动传入文件名和行号 */
#define LOG_DEBUG(...) log_print_impl(LOG_DEBUG, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_INFO(...)  log_print_impl(LOG_INFO,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_WARN(...)  log_print_impl(LOG_WARN,  __FILE__, __LINE__, __VA_ARGS__)
#define LOG_ERROR(...) log_print_impl(LOG_ERROR, __FILE__, __LINE__, __VA_ARGS__)
#define LOG_FATAL(...) log_print_impl(LOG_FATAL, __FILE__, __LINE__, __VA_ARGS__)

/* 异常抛出宏：自动传入文件名和行号，打印后退出 */
#define THROW_ERROR(...) log_throw_impl(__FILE__, __LINE__, __VA_ARGS__)

#endif /* LUMYR_LOG_H */
