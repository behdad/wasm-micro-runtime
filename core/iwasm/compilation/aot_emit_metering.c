/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "aot_emit_metering.h"
#include "../aot/aot_runtime.h"

#if WASM_ENABLE_INSTRUCTION_METERING != 0

/* exec_env->instructions_left is so many words into the exec env, in a
   runtime for any target: see the bh_static_assert in aot_runtime.c */
#define INSTRUCTIONS_LEFT_WORD_INDEX 13

/* The run of opcodes which the code pays for at once isn't made longer
   than this */
#define RUN_COUNT_MAX ((uint64)INT32_MAX)

/* Load the local variable from the exec env */
static bool
load_count(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    LLVMValueRef left;

    if (!(left = LLVMBuildLoad2(comp_ctx->builder, INTPTR_T_TYPE,
                                func_ctx->instructions_left_addr,
                                "instructions_left"))
        || !LLVMBuildStore(comp_ctx->builder, left,
                           func_ctx->instructions_left)) {
        aot_set_last_error("llvm build load or store failed.");
        return false;
    }
    return true;
}

bool
aot_metering_create_count(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    LLVMValueRef index = I32_CONST(INSTRUCTIONS_LEFT_WORD_INDEX), addr;

    if (!comp_ctx->enable_instruction_metering)
        return true;

    CHECK_LLVM_CONST(index);

    if (!(addr = LLVMBuildInBoundsGEP2(comp_ctx->builder, OPQ_PTR_TYPE,
                                       func_ctx->exec_env, &index, 1,
                                       "instructions_left_addr"))
        || !(func_ctx->instructions_left_addr =
                 LLVMBuildBitCast(comp_ctx->builder, addr, INTPTR_T_PTR_TYPE,
                                  "instructions_left_ptr"))) {
        aot_set_last_error("llvm build in bounds gep or bit cast failed.");
        goto fail;
    }

    if (!(func_ctx->instructions_left = LLVMBuildAlloca(
              comp_ctx->builder, INTPTR_T_TYPE, "instructions_left_var"))) {
        aot_set_last_error("llvm build alloca failed.");
        goto fail;
    }

    return load_count(comp_ctx, func_ctx);
fail:
    return false;
}

/*
 * Get the block of the function where the code goes when the call has no
 * instructions left: it throws the exception and returns. The function
 * that it calls needs the exec env only, so the code of a function which
 * throws no other exception doesn't keep the module instance at hand.
 */
static LLVMBasicBlockRef
get_limit_exceeded_block(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMBasicBlockRef block_curr = LLVMGetInsertBlock(builder);
    LLVMValueRef func, value, param_values[1];
    LLVMTypeRef param_types[1], ret_type, func_type, func_ptr_type;

    if (func_ctx->instruction_limit_exceeded_block)
        return func_ctx->instruction_limit_exceeded_block;

    if (!(func_ctx->instruction_limit_exceeded_block =
              LLVMAppendBasicBlockInContext(comp_ctx->context, func_ctx->func,
                                            "instruction_limit_exceeded"))) {
        aot_set_last_error("add LLVM basic block failed.");
        goto fail;
    }
    LLVMPositionBuilderAtEnd(builder,
                             func_ctx->instruction_limit_exceeded_block);

    param_types[0] = comp_ctx->exec_env_type;
    ret_type = VOID_TYPE;
    GET_AOT_FUNCTION(wasm_runtime_instruction_limit_exceeded, 1);

    param_values[0] = func_ctx->exec_env;
    if (!LLVMBuildCall2(builder, func_type, func, param_values, 1, "")) {
        aot_set_last_error("llvm build call failed.");
        goto fail;
    }
    if (!aot_build_zero_function_ret(comp_ctx, func_ctx,
                                     func_ctx->aot_func->func_type))
        goto fail;

    LLVMPositionBuilderAtEnd(builder, block_curr);
    return func_ctx->instruction_limit_exceeded_block;
fail:
    return NULL;
}

/*
 * Build the code which takes a count off the instructions that the exec
 * env has left, and throws the exception if there were fewer left.
 * Return the insn that has the count as its second operand, NULL if
 * failed.
 */
static LLVMValueRef
build_meter(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
            LLVMValueRef count)
{
    LLVMBuilderRef builder = comp_ctx->builder;
    LLVMBasicBlockRef block_curr = LLVMGetInsertBlock(builder);
    LLVMBasicBlockRef limit_exceeded_block, ran_out_block, left_block;
    LLVMValueRef zero = LLVMConstInt(INTPTR_T_TYPE, 0, false);
    LLVMValueRef left, insn, store, cmp, br, func, value, param_values[1];
    LLVMTypeRef param_types[1], ret_type, func_type, func_ptr_type;
    /* A machine word of 64 bits holds all of a limit, and a call without
       a limit doesn't get to the end of INTPTR_MAX: when the count has
       run out, the call has nothing left. A machine word of 32 bits may
       not hold all of it, and the runtime is asked. */
    bool is_refilled = comp_ctx->pointer_size != sizeof(uint64);
    bool commit_ip =
        comp_ctx->aot_frame && comp_ctx->call_stack_features.trap_ip;

    CHECK_LLVM_CONST(zero);

    if (!(left =
              LLVMBuildLoad2(builder, INTPTR_T_TYPE,
                             func_ctx->instructions_left, "instructions_left"))
        || !(insn = LLVMBuildSub(builder, left, count, "instructions_left"))
        || !LLVMBuildStore(builder, insn, func_ctx->instructions_left)
        || !(store = LLVMBuildStore(builder, insn,
                                    func_ctx->instructions_left_addr))
        || !(cmp = LLVMBuildICmp(builder, LLVMIntSLT, insn, zero,
                                 "instructions_ran_out"))) {
        aot_set_last_error("llvm build insn failed.");
        goto fail;
    }
    /* The store into the exec env mustn't be put off or left out: the
       code may be left by a signal at any memory access */
    LLVMSetVolatile(store, true);

    if (!(limit_exceeded_block = get_limit_exceeded_block(comp_ctx, func_ctx)))
        goto fail;

    if (!(left_block = LLVMAppendBasicBlockInContext(
              comp_ctx->context, func_ctx->func, "instructions_left"))) {
        aot_set_last_error("add LLVM basic block failed.");
        goto fail;
    }
    LLVMMoveBasicBlockAfter(left_block, block_curr);

    ran_out_block = limit_exceeded_block;
    if ((is_refilled || commit_ip)
        && !(ran_out_block = LLVMAppendBasicBlockInContext(
                 comp_ctx->context, func_ctx->func, "instructions_ran_out"))) {
        aot_set_last_error("add LLVM basic block failed.");
        goto fail;
    }

    if (!(br = LLVMBuildCondBr(builder, cmp, ran_out_block, left_block))) {
        aot_set_last_error("llvm build cond br failed.");
        goto fail;
    }
    /* The weights which llvm MDBuilder::createUnlikelyBranchWeights
       assigns */
    aot_set_cond_br_weights(comp_ctx, br, 1, (1 << 20) - 1);

    if (ran_out_block != limit_exceeded_block) {
        LLVMPositionBuilderAtEnd(builder, ran_out_block);

        /* The call stack is to show where the call was stopped */
        if (commit_ip
            && !aot_gen_commit_sp_ip(comp_ctx->aot_frame, false, true))
            goto fail;

        if (is_refilled) {
            param_types[0] = comp_ctx->exec_env_type;
            ret_type = INTPTR_T_TYPE;
            GET_AOT_FUNCTION(wasm_runtime_refill_instruction_count, 1);

            param_values[0] = func_ctx->exec_env;
            if (!(left = LLVMBuildCall2(builder, func_type, func, param_values,
                                        1, "instructions_left"))
                || !LLVMBuildStore(builder, left, func_ctx->instructions_left)
                || !(cmp = LLVMBuildICmp(builder, LLVMIntSLT, left, zero,
                                         "instruction_limit_exceeded"))
                || !LLVMBuildCondBr(builder, cmp, limit_exceeded_block,
                                    left_block)) {
                aot_set_last_error("llvm build insn failed.");
                goto fail;
            }
        }
        else if (!LLVMBuildBr(builder, limit_exceeded_block)) {
            aot_set_last_error("llvm build br failed.");
            goto fail;
        }
    }

    LLVMPositionBuilderAtEnd(builder, left_block);
    return insn;
fail:
    return NULL;
}

/* Add to the count of the run, and begin a run if there is none */
static bool
count_in_run(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx, uint32 count)
{
    LLVMValueRef value;

    if (func_ctx->metering_insn
        && func_ctx->metering_count + count > RUN_COUNT_MAX)
        aot_metering_end_run(func_ctx);

    if (!func_ctx->metering_frame_counted) {
        /* The first run of a function also pays for its local variables,
           which the code sets to zero, like a call does in the
           interpreter: one instruction for every
           WASM_METERING_BYTES_PER_INSTRUCTION bytes of them */
        func_ctx->metering_count += func_ctx->aot_func->local_cell_num * 4
                                    / WASM_METERING_BYTES_PER_INSTRUCTION;
        func_ctx->metering_frame_counted = true;
    }

    func_ctx->metering_count += count;
    value = LLVMConstInt(INTPTR_T_TYPE, func_ctx->metering_count, false);
    CHECK_LLVM_CONST(value);

    if (func_ctx->metering_insn)
        LLVMSetOperand(func_ctx->metering_insn, 1, value);
    else if (!(func_ctx->metering_insn =
                   build_meter(comp_ctx, func_ctx, value)))
        goto fail;

    return true;
fail:
    return false;
}

bool
aot_metering_count_opcode(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    if (!comp_ctx->enable_instruction_metering)
        return true;

    if (func_ctx->metering_prepaid) {
        /* The loop or the conditional branch before this opcode has paid
           for it */
        func_ctx->metering_prepaid = false;
        return true;
    }

    return count_in_run(comp_ctx, func_ctx, 1);
}

bool
aot_metering_count_target(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    if (!comp_ctx->enable_instruction_metering)
        return true;

    if (!count_in_run(comp_ctx, func_ctx, 1))
        return false;

    aot_metering_end_run(func_ctx);
    return true;
}

bool
aot_metering_count_next(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    if (!comp_ctx->enable_instruction_metering)
        return true;

    if (!count_in_run(comp_ctx, func_ctx, 1))
        return false;

    aot_metering_end_run(func_ctx);
    func_ctx->metering_prepaid = true;
    return true;
}

void
aot_metering_end_run(AOTFuncContext *func_ctx)
{
    func_ctx->metering_insn = NULL;
    func_ctx->metering_count = 0;
}

bool
aot_metering_end_call(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx)
{
    if (!comp_ctx->enable_instruction_metering)
        return true;

    aot_metering_end_run(func_ctx);
    return load_count(comp_ctx, func_ctx);
}

bool
aot_metering_count_units(AOTCompContext *comp_ctx, AOTFuncContext *func_ctx,
                         LLVMValueRef units, uint32 units_per_instruction)
{
    LLVMTypeRef units_type = LLVMTypeOf(units);
    LLVMValueRef count, count_max, cmp, shift_value;
    uint32 shift = 0;

    if (!comp_ctx->enable_instruction_metering)
        return true;

    while (((uint32)1 << shift) < units_per_instruction)
        shift++;

    if (LLVMIsAConstantInt(units)) {
        uint64 const_count = LLVMConstIntGetZExtValue(units) >> shift;

        /* It is known now and can be paid for with the opcodes */
        if (const_count == 0)
            return true;
        if (const_count <= RUN_COUNT_MAX)
            return count_in_run(comp_ctx, func_ctx, (uint32)const_count);
    }

    shift_value = LLVMConstInt(units_type, shift, false);
    CHECK_LLVM_CONST(shift_value);
    if (!(count = LLVMBuildLShr(comp_ctx->builder, units, shift_value,
                                "units_count"))) {
        aot_set_last_error("llvm build lshr failed.");
        goto fail;
    }

    if (LLVMGetIntTypeWidth(units_type) > LLVMGetIntTypeWidth(INTPTR_T_TYPE)) {
        /* More than the count can hold is more than there is left */
        count_max = LLVMConstInt(units_type, RUN_COUNT_MAX, false);
        CHECK_LLVM_CONST(count_max);
        if (!(cmp = LLVMBuildICmp(comp_ctx->builder, LLVMIntUGT, count,
                                  count_max, "units_count_cmp"))
            || !(count = LLVMBuildSelect(comp_ctx->builder, cmp, count_max,
                                         count, "units_count_min"))
            || !(count = LLVMBuildTrunc(comp_ctx->builder, count, INTPTR_T_TYPE,
                                        "units_count_trunc"))) {
            aot_set_last_error("llvm build insn failed.");
            goto fail;
        }
    }
    else if (LLVMGetIntTypeWidth(units_type)
             < LLVMGetIntTypeWidth(INTPTR_T_TYPE)) {
        if (!(count = LLVMBuildZExt(comp_ctx->builder, count, INTPTR_T_TYPE,
                                    "units_count_ext"))) {
            aot_set_last_error("llvm build zext failed.");
            goto fail;
        }
    }

    return build_meter(comp_ctx, func_ctx, count) ? true : false;
fail:
    return false;
}

#endif /* end of WASM_ENABLE_INSTRUCTION_METERING != 0 */
