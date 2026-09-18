/*
 * 字节码指令发射模块实现
 *
 * 本模块提供公共的字节码指令发射函数，供 IR 编译、代码生成等模块使用。
 */

#include "ir_emit.h"

/* 发射一条字节码指令 */
void emit(Ctx* c, OpCode op, int a, int b) {
    if(op == OPC_PRINT_BIGINT || op == OPC_PRINT_PTR) {
    }
    bf_emit(c->fn, op, a, b);
}

/* 发射一条字节码指令，并返回指令在字节码流中的位置（用于后续 patch） */
int emit_here(Ctx* c, OpCode op, int a, int b) {
    return bf_emit_here(c->fn, op, a, b);
}

/* patch 指令的跳转目标为当前位置 */
void patch_to(Ctx* c, int pos) {
    bf_patch(c->fn, pos, here(c));
}

/* 获取当前字节码流的位置（下一条指令的位置） */
int here(Ctx* c) {
    return c->fn->code_len;
}
