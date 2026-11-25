/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "jit_emit_metering.h"
#include "jit_emit_exception.h"
#include "../jit_frontend.h"
#include "../../interpreter/wasm_opcode.h"
#include "../../interpreter/wasm_runtime.h"

#if WASM_ENABLE_INSTRUCTION_METERING != 0

bool
jit_metering_count_opcode(JitCompContext *cc, uint8 opcode)
{
    uint32 count = 1;

    cc->metering_opcode = opcode;
    cc->metering_basic_block = cc->cur_basic_block;

    switch (opcode) {
        case WASM_OP_LOOP:
        case EXT_OP_LOOP:
        case WASM_OP_ELSE:
        case WASM_OP_BR:
        case WASM_OP_BR_IF:
        case WASM_OP_BR_TABLE:
        case EXT_OP_BR_TABLE_CACHE:
            /* These opcodes pay for the opcode which the interpreter
               dispatches after them as well, so that an opcode which is
               reached in several ways needn't be paid for where they
               join:
               - a branch pays for the `end` of the block it branches
                 to, or for the first opcode of the loop it branches to
               - a br_if which doesn't branch, and a loop, pay for the
                 opcode which follows them
               - an else, which only the end of the `if` branch gets
                 to, pays for the `end` it jumps to
               The `end` after an `if` whose condition is false and the
               else that is jumped over aren't dispatched, and nothing
               pays for them. */
            count = 2;
            break;
        default:
            break;
    }

    if (cc->metering_prepaid) {
        /* The loop or br_if before this opcode has paid for it */
        cc->metering_prepaid = false;
        count--;
    }

    if (count == 0)
        return true;

    /* The count of a run is a 32-bit immediate of its METER insn */
    if (cc->metering_insn && cc->metering_count + count > (uint32)INT32_MAX)
        jit_metering_end_run(cc);

    if (!cc->metering_frame_counted) {
        /* The first run of a function also pays for the local variables
           that the code at the entry of the function has set to zero,
           like a call does in the interpreter: one instruction for every
           WASM_METERING_BYTES_PER_INSTRUCTION bytes of them */
        count += cc->cur_wasm_func->local_cell_num * 4
                 / WASM_METERING_BYTES_PER_INSTRUCTION;
        cc->metering_frame_counted = true;
    }

    if (!cc->metering_insn) {
        /* Begin a run: the count is filled in when the run ends */
        cc->metering_insn = GEN_INSN(METER, cc->cmp_reg, NEW_CONST(I32, 0));
        if (!cc->metering_insn
            || !jit_emit_exception(cc, EXCE_INSTRUCTION_LIMIT_EXCEEDED,
                                   JIT_OP_BLTS, cc->cmp_reg, NULL))
            return false;
    }

    cc->metering_count += count;
    return true;
}

void
jit_metering_end_opcode(JitCompContext *cc)
{
    uint8 opcode = cc->metering_opcode;

    /* What follows the opcode isn't always executed together with what
       is before it:
       - when the translation has moved on to another basic block
       - after a br_if, which may branch away
       - after a call: the callee, or a native function and what it
         calls, then runs with exactly the instructions before the call
         paid for, like in the interpreter */
    if (cc->cur_basic_block != cc->metering_basic_block
        || opcode == WASM_OP_BR_IF || opcode == WASM_OP_CALL
        || opcode == WASM_OP_CALL_INDIRECT || opcode == WASM_OP_RETURN_CALL
        || opcode == WASM_OP_RETURN_CALL_INDIRECT)
        jit_metering_end_run(cc);

    if (opcode == WASM_OP_LOOP || opcode == EXT_OP_LOOP
        || opcode == WASM_OP_BR_IF)
        cc->metering_prepaid = true;
}

void
jit_metering_end_run(JitCompContext *cc)
{
    if (cc->metering_insn) {
        *(jit_insn_opnd(cc->metering_insn, 1)) =
            NEW_CONST(I32, (int32)cc->metering_count);
        cc->metering_insn = NULL;
        cc->metering_count = 0;
    }
}

bool
jit_metering_count_bytes(WASMModuleInstance *module_inst, uint64 bytes)
{
    /* The exec env of the call into the module instance that is running:
       the jitted code loads its count again after a native function has
       returned */
    bh_assert(module_inst->e->running_exec_env);
    if (wasm_exec_env_count_bytes(module_inst->e->running_exec_env, bytes))
        return true;

    wasm_set_exception_with_id(module_inst, EXCE_INSTRUCTION_LIMIT_EXCEEDED);
    return false;
}

#endif /* end of WASM_ENABLE_INSTRUCTION_METERING != 0 */
