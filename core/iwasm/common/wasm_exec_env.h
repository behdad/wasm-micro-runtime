/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#ifndef _WASM_EXEC_ENV_H
#define _WASM_EXEC_ENV_H

#include "bh_assert.h"
#include "wasm_suspend_flags.h"
#if WASM_ENABLE_INTERP != 0
#include "../interpreter/wasm.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

struct WASMModuleInstanceCommon;
struct WASMInterpFrame;

#if WASM_ENABLE_THREAD_MGR != 0
typedef struct WASMCluster WASMCluster;
#if WASM_ENABLE_DEBUG_INTERP != 0
typedef struct WASMCurrentEnvStatus WASMCurrentEnvStatus;
#endif
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK
typedef struct WASMJmpBuf {
    struct WASMJmpBuf *prev;
    korp_jmpbuf jmpbuf;
} WASMJmpBuf;
#endif

/* Execution environment */
typedef struct WASMExecEnv {
    /* Next thread's exec env of a WASM module instance. */
    struct WASMExecEnv *next;

    /* Current interpreter/AOT frame of current thread */
    struct WASMInterpFrame *cur_frame;

    /* Note: field module_inst, argv_buf, native_stack_boundary,
       suspend_flags, aux_stack_boundary, aux_stack_bottom, and
       native_symbol are used by AOTed code, don't change the
       places of them */

    /* The WASM module instance of current thread */
    struct WASMModuleInstanceCommon *module_inst;

#if WASM_ENABLE_AOT != 0
    uint32 *argv_buf;
#endif

    /* The boundary of native stack. When runtime detects that native
       frame may overrun this boundary, it throws stack overflow
       exception. */
    uint8 *native_stack_boundary;

    /* Used to terminate or suspend current thread */
    WASMSuspendFlags suspend_flags;

    /* Auxiliary stack boundary */
    uintptr_t aux_stack_boundary;

    /* Auxiliary stack bottom */
    uintptr_t aux_stack_bottom;

#if WASM_ENABLE_AOT != 0
    /* Native symbol list, reserved */
    void **native_symbol;
#endif

    /*
     * The lowest stack pointer value observed.
     * Assumption: native stack grows to the lower address.
     */
    uint8 *native_stack_top_min;

    struct {
        /* The top boundary of the stack. */
        uint8 *top_boundary;
        /* The top to of the wasm stack which is free. */
        uint8 *top;
        /* The bottom of the wasm stack. */
        uint8 *bottom;
    } wasm_stack;

#if WASM_ENABLE_INSTRUCTION_METERING != 0
    /* instructions to execute */
    int64 instructions_to_execute;
    /* What the outermost call into wasm code that is running on this
       exec env has left of its instructions beyond instructions_left,
       which is filled up from here when it has run out: a machine
       word of 32 bits can't hold all of a limit. INT64_MAX if the call
       has no limit, which is never used up. */
    int64 instructions_reserve;
    /* What the outermost call into wasm code that is running on this
       exec env has left of its instructions, as far as a machine word
       holds it. The calls nested in it share the count. A call without
       a limit counts down as well, from INTPTR_MAX. */
    intptr_t instructions_left;
#endif

#if WASM_ENABLE_FAST_JIT != 0
    /**
     * Cache for
     * - jit native operations in 32-bit target which hasn't 64-bit
     *   int/float registers, mainly for the operations of double and int64,
     *   such as F64TOI64, F32TOI64, I64 MUL/REM, and so on.
     * - SSE instructions.
     **/
    uint64 jit_cache[2];
#endif

#if WASM_ENABLE_THREAD_MGR != 0
    /* thread return value */
    void *thread_ret_value;

    /* Must be provided by thread library */
    void *(*thread_start_routine)(void *);
    void *thread_arg;

    /* pointer to the cluster */
    WASMCluster *cluster;

    /* used to support debugger */
    korp_mutex wait_lock;
    korp_cond wait_cond;
    /* the count of threads which are joining current thread */
    uint32 wait_count;

    /* whether current thread is detached */
    bool thread_is_detached;

    /* whether the aux stack is allocated */
    bool is_aux_stack_allocated;
#endif

#if WASM_ENABLE_GC != 0
    /* Current local object reference variable */
    struct WASMLocalObjectRef *cur_local_object_ref;
#endif

#if WASM_ENABLE_DEBUG_INTERP != 0
    WASMCurrentEnvStatus *current_status;
#endif

    /* attachment for native function */
    void *attachment;

    void *user_data;

    /* The boundary of native stack set by host embedder. It is used
       if it is not NULL when calling wasm functions. */
    uint8 *user_native_stack_boundary;

    /* The native thread handle of current thread */
    korp_tid handle;

#if WASM_ENABLE_INTERP != 0 && WASM_ENABLE_FAST_INTERP == 0
    BlockAddr block_addr_cache[BLOCK_ADDR_CACHE_SIZE][BLOCK_ADDR_CONFLICT_SIZE];
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK
    WASMJmpBuf *jmpbuf_stack_top;
    /* One guard page for the exception check */
    uint8 *exce_check_guard_page;
#endif

#if WASM_ENABLE_MEMORY_PROFILING != 0
    uint32 max_wasm_stack_used;
#endif

    /* The WASM stack size */
    uint32 wasm_stack_size;

    /* The WASM stack of current thread */
    union {
        uint64 __make_it_8_byte_aligned_;
        /* The WASM stack. */
        uint8 bottom[1];
    } wasm_stack_u;
} WASMExecEnv;

#if WASM_ENABLE_MEMORY_PROFILING != 0
#define RECORD_STACK_USAGE(e, p)               \
    do {                                       \
        if ((e)->native_stack_top_min > (p)) { \
            (e)->native_stack_top_min = (p);   \
        }                                      \
    } while (0)
#else
#define RECORD_STACK_USAGE(e, p) (void)0
#endif

WASMExecEnv *
wasm_exec_env_create_internal(struct WASMModuleInstanceCommon *module_inst,
                              uint32 stack_size);

void
wasm_exec_env_destroy_internal(WASMExecEnv *exec_env);

WASMExecEnv *
wasm_exec_env_create(struct WASMModuleInstanceCommon *module_inst,
                     uint32 stack_size);

void
wasm_exec_env_destroy(WASMExecEnv *exec_env);

static inline bool
wasm_exec_env_is_aux_stack_managed_by_runtime(WASMExecEnv *exec_env)
{
    return exec_env->aux_stack_boundary != 0 || exec_env->aux_stack_bottom != 0;
}

/**
 * Allocate a WASM frame from the WASM stack.
 *
 * @param exec_env the current execution environment
 * @param size size of the WASM frame, it must be a multiple of 4
 *
 * @return the WASM frame if there is enough space in the stack area
 * with a protection area, NULL otherwise
 */
static inline void *
wasm_exec_env_alloc_wasm_frame(WASMExecEnv *exec_env, unsigned size)
{
    uint8 *addr = exec_env->wasm_stack.top;

    bh_assert(!(size & 3));

    /* For classic interpreter, the outs area doesn't contain the const cells,
       its size cannot be larger than the frame size, so here checking stack
       overflow with multiplying by 2 is enough. For fast interpreter, since
       the outs area contains const cells, its size may be larger than current
       frame size, we should check again before putting the function arguments
       into the outs area. */
    if (size * 2
        > (uint32)(uintptr_t)(exec_env->wasm_stack.top_boundary - addr)) {
        /* WASM stack overflow. */
        return NULL;
    }

    exec_env->wasm_stack.top += size;

#if WASM_ENABLE_MEMORY_PROFILING != 0
    {
        uint32 wasm_stack_used =
            exec_env->wasm_stack.top - exec_env->wasm_stack.bottom;
        if (wasm_stack_used > exec_env->max_wasm_stack_used)
            exec_env->max_wasm_stack_used = wasm_stack_used;
    }
#endif
    return addr;
}

static inline void
wasm_exec_env_free_wasm_frame(WASMExecEnv *exec_env, void *prev_top)
{
    bh_assert((uint8 *)prev_top >= exec_env->wasm_stack.bottom);
    exec_env->wasm_stack.top = (uint8 *)prev_top;
}

/**
 * Get the current WASM stack top pointer.
 *
 * @param exec_env the current execution environment
 *
 * @return the current WASM stack top pointer
 */
static inline void *
wasm_exec_env_wasm_stack_top(WASMExecEnv *exec_env)
{
    return exec_env->wasm_stack.top;
}

/**
 * Set the current frame pointer.
 *
 * @param exec_env the current execution environment
 * @param frame the WASM frame to be set for the current exec env
 */
static inline void
wasm_exec_env_set_cur_frame(WASMExecEnv *exec_env,
                            struct WASMInterpFrame *frame)
{
    exec_env->cur_frame = frame;
}

/**
 * Get the current frame pointer.
 *
 * @param exec_env the current execution environment
 *
 * @return the current frame pointer
 */
static inline struct WASMInterpFrame *
wasm_exec_env_get_cur_frame(WASMExecEnv *exec_env)
{
    return exec_env->cur_frame;
}

#if WASM_ENABLE_INSTRUCTION_METERING != 0
/**
 * Get what the call that is running on the exec env has left of its
 * instructions.
 *
 * @param exec_env the execution environment the call is made with
 *
 * @return what the call has left, -1 if it has no limit
 */
static inline int64
wasm_exec_env_get_instructions_left(WASMExecEnv *exec_env)
{
    if (exec_env->instructions_reserve == INT64_MAX)
        return -1;
    /* The count is below zero after the limit was reached */
    return (exec_env->instructions_left > 0 ? exec_env->instructions_left : 0)
           + exec_env->instructions_reserve;
}

/**
 * Set what the call that is running on the exec env has left of its
 * instructions.
 *
 * @param exec_env the execution environment the call is made with
 * @param instructions_left what the call has left, not negative
 */
static inline void
wasm_exec_env_set_instructions_left(WASMExecEnv *exec_env,
                                    int64 instructions_left)
{
    exec_env->instructions_left = instructions_left < INTPTR_MAX
                                      ? (intptr_t)instructions_left
                                      : INTPTR_MAX;
    exec_env->instructions_reserve =
        instructions_left - exec_env->instructions_left;
}

/* An instruction that handles a block of memory, like memory.fill, counts
   one more instruction for every so many bytes of it, and an instruction
   that handles table elements takes each of them for so many bytes */
#define WASM_METERING_BYTES_PER_INSTRUCTION 64
#define WASM_METERING_BYTES_PER_TABLE_ELEM 8

/**
 * Count what an instruction of the call that is running on the exec env
 * counts for a block of memory that it handles, beyond the one which
 * every instruction counts.
 *
 * @param exec_env the execution environment the call is made with
 * @param bytes the size of the block
 *
 * @return true if the call had so many instructions left, false if it
 *         hadn't, and then it has none left
 */
static inline bool
wasm_exec_env_count_bytes(WASMExecEnv *exec_env, uint64 bytes)
{
    int64 left = wasm_exec_env_get_instructions_left(exec_env);
    uint64 count = bytes / WASM_METERING_BYTES_PER_INSTRUCTION;

    if (left < 0) {
        /* A call without a limit counts down as well, and never has
           too few instructions left */
        exec_env->instructions_left =
            exec_env->instructions_left > 0
                    && (uint64)exec_env->instructions_left > count
                ? exec_env->instructions_left - (intptr_t)count
                : 0;
        return true;
    }

    if ((uint64)left < count) {
        wasm_exec_env_set_instructions_left(exec_env, 0);
        return false;
    }
    wasm_exec_env_set_instructions_left(exec_env, left - (int64)count);
    return true;
}

/**
 * Start to count the instructions of a call into WASM code. The outermost
 * call that is running on the exec env gets the whole instruction count
 * limit. The calls nested in it, which a native function or the runtime
 * makes on the same exec env, go on with what it has left.
 *
 * @param exec_env the execution environment the call is made with
 * @param limit_enforced whether the limit is enforced in the running mode
 *        of the module instance that is called, see
 *        wasm_runtime_is_instruction_count_limit_enforced(). If it isn't,
 *        the call has no limit: what does count the instructions in such
 *        a mode, like the code of Fast JIT before a function tiers up to
 *        LLVM JIT, mustn't end the call
 */
static inline void
wasm_exec_env_start_instruction_count(WASMExecEnv *exec_env,
                                      bool limit_enforced)
{
    /* Only the outermost call finds no frame in the exec env */
    if (!exec_env->cur_frame) {
        int64 limit = exec_env->instructions_to_execute;

        if (limit_enforced && limit >= 0) {
            wasm_exec_env_set_instructions_left(exec_env, limit);
        }
        else {
            exec_env->instructions_left = INTPTR_MAX;
            exec_env->instructions_reserve = INT64_MAX;
        }
    }
}

/**
 * Fill up the count of the instructions that a call has left, when it
 * has run out, from what the call has left beyond the count.
 *
 * @param exec_env the execution environment the call is made with
 *
 * @return what the count goes on with, 0 if the call has nothing left
 */
static inline intptr_t
wasm_exec_env_refill_instruction_count(WASMExecEnv *exec_env)
{
    int64 reserve = exec_env->instructions_reserve;
    intptr_t count = reserve < INTPTR_MAX ? (intptr_t)reserve : INTPTR_MAX;

    /* A call without a limit doesn't use it up */
    if (reserve != INT64_MAX)
        exec_env->instructions_reserve = reserve - count;
    return count;
}
#endif

struct WASMModuleInstanceCommon *
wasm_exec_env_get_module_inst(WASMExecEnv *exec_env);

void
wasm_exec_env_set_module_inst(
    WASMExecEnv *exec_env, struct WASMModuleInstanceCommon *const module_inst);

void
wasm_exec_env_restore_module_inst(
    WASMExecEnv *exec_env, struct WASMModuleInstanceCommon *const module_inst);

void
wasm_exec_env_set_thread_info(WASMExecEnv *exec_env);

#if WASM_ENABLE_THREAD_MGR != 0
void *
wasm_exec_env_get_thread_arg(WASMExecEnv *exec_env);

void
wasm_exec_env_set_thread_arg(WASMExecEnv *exec_env, void *thread_arg);
#endif

#ifdef OS_ENABLE_HW_BOUND_CHECK
void
wasm_exec_env_push_jmpbuf(WASMExecEnv *exec_env, WASMJmpBuf *jmpbuf);

WASMJmpBuf *
wasm_exec_env_pop_jmpbuf(WASMExecEnv *exec_env);
#endif

#ifdef __cplusplus
}
#endif

#endif /* end of _WASM_EXEC_ENV_H */
