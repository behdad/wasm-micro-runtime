/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _AOT_EMIT_METERING_H_
#define _AOT_EMIT_METERING_H_

#include "aot_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#if WASM_ENABLE_INSTRUCTION_METERING != 0

/*
 * Instruction metering in the code of LLVM JIT and AOT counts what the
 * classic interpreter counts: one instruction for every opcode it
 * dispatches, and more for the instructions that handle a block of
 * memory or a part of a table, and for the local variables of a
 * function that is called.
 *
 * The code doesn't count the opcodes one by one: it pays for a run of
 * them at once, before it executes them. A run is a sequence of opcodes
 * that are executed one after another whenever the first one is. The
 * code in front of the run takes the number of its opcodes off the
 * instructions that the exec env has left, and if there were fewer left
 * than that, it throws the exception "instruction limit exceeded".
 *
 * An opcode which the interpreter dispatches after a branch is paid for
 * before the branch, so that an opcode which is reached in several ways
 * needn't be paid for where they join:
 * - a br, a br_table and a conditional branch that branches pay for the
 *   `end` of the block they branch to, or for the first opcode of the
 *   loop they branch to
 * - a conditional branch that doesn't branch, and a loop, pay for the
 *   opcode which follows them
 * - an else, which only the end of the `if` branch gets to, pays for
 *   the `end` it jumps to
 * The `end` after an `if` whose condition is false and the else that is
 * jumped over aren't dispatched, and nothing pays for them.
 *
 * What the exec env has left is kept in a local variable of the
 * function, which is only a cache: each change of it is stored into the
 * exec env at once, so that the exec env is up to date whichever way the
 * code is left. The variable is loaded where the count may have changed
 * without the function: at its entry and after each call.
 */

/**
 * Create the local variable of a function which caches the instructions
 * that the exec env has left, and load it. It is called when the
 * function context is created, with the builder at the entry block of
 * the function.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_create_count(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx);

/**
 * Count the opcode that is going to be translated in the run of opcodes
 * which the code pays for at once, and begin a run if there is none.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_count_opcode(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx);

/**
 * Count the opcode which the interpreter dispatches after the br, the
 * br_table or the else that is being translated, and end the run.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_count_target(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx);

/**
 * Count the opcode which the interpreter dispatches after the loop or
 * the conditional branch that is being translated, and end the run. The
 * opcode which follows it has then been paid for.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_count_next(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx);

/**
 * End the run of opcodes which the code pays for at once, if there is
 * one: what is translated next isn't always executed together with
 * what was translated before.
 *
 * @param func_ctx the function context
 */
void
aot_metering_end_run(AOTFuncContext *func_ctx);

/**
 * End the run after the translation of a call, and load the local
 * variable again: the callee, or a native function and what it has
 * called, has counted in the exec env.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_end_call(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx);

/**
 * Count what an instruction counts for what it is going to handle, beyond
 * the one which every instruction counts: one instruction for every so
 * many units of it.
 *
 * @param comp_ctx the compilation context
 * @param func_ctx the function context
 * @param units the number of the bytes or table elements, an integer
 * @param units_per_instruction how many of them count one instruction,
 *        a power of 2
 *
 * @return true if success, false otherwise
 */
bool
aot_metering_count_units(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                         LLVMValueRef units, uint32 units_per_instruction);

#endif /* end of WASM_ENABLE_INSTRUCTION_METERING != 0 */

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _AOT_EMIT_METERING_H_ */
