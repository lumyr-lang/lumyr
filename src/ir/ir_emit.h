#ifndef LUMYR_IR_EMIT_H
#define LUMYR_IR_EMIT_H

#include "ir_types.h"
#include "bytecode.h"

/*
 * 字节码指令发射模块
 *
 * 本模块提供公共的字节码指令发射函数，供 IR 编译、代码生成等模块使用。
 */

/* 发射一条字节码指令 */
void emit(Ctx* c, OpCode op, int a, int b);

/* 发射一条字节码指令，并返回指令在字节码流中的位置（用于后续 patch） */
int emit_here(Ctx* c, OpCode op, int a, int b);

/* patch 指令的跳转目标为当前位置 */
void patch_to(Ctx* c, int pos);

/* 获取当前字节码流的位置（下一条指令的位置） */
int here(Ctx* c);

#endif /* LUMYR_IR_EMIT_H */
