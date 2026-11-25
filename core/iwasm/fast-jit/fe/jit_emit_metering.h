/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _JIT_EMIT_METERING_H_
#define _JIT_EMIT_METERING_H_

#include "../jit_compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#if WASM_ENABLE_INSTRUCTION_METERING != 0

/*
 * Instruction metering in the jitted code counts what the classic
 * interpreter counts: one instruction for every opcode it dispatches.
 *
 * The jitted code doesn't count the opcodes one by one: it pays for a
 * run of them at once, before it executes them. A run is a sequence
 * of opcodes that are executed one after another whenever the first
 * one is. A METER insn in front of the run takes the number of its
 * opcodes off the instructions that the exec env has left, and the
 * branch insn which follows it throws the exception "instruction limit
 * exceeded" if there were fewer left than that.
 *
 * The translation of each opcode is put between a call of
 * jit_metering_count_opcode() and one of jit_metering_end_opcode(),
 * and jit_metering_end_run() is called after the last opcode.
 */

/**
 * Count an opcode that is going to be translated in the run of opcodes
 * which the jitted code pays for at once, and begin a run if there is
 * none.
 *
 * @param cc the compilation context
 * @param opcode the opcode
 *
 * @return true if success, false otherwise
 */
bool
jit_metering_count_opcode(JitCompContext *cc, uint8 opcode);

/**
 * End the run of opcodes after the opcode that was translated, if what
 * follows it isn't always executed together with what is before it.
 *
 * @param cc the compilation context
 */
void
jit_metering_end_opcode(JitCompContext *cc);

/**
 * End the run of opcodes which the jitted code pays for at once, if
 * there is one.
 *
 * @param cc the compilation context
 */
void
jit_metering_end_run(JitCompContext *cc);

#endif /* end of WASM_ENABLE_INSTRUCTION_METERING != 0 */

#ifdef __cplusplus
} /* end of extern "C" */
#endif

#endif /* end of _JIT_EMIT_METERING_H_ */
