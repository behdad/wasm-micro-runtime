/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <climits>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "wasm_runtime_common.h"
#include "bh_platform.h"
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT != 0 \
    && WASM_ENABLE_LAZY_JIT != 0
#include "wasm_runtime.h"
#include "jit_compiler.h"
#endif

namespace {

/* What a call ends with when it reaches the limit */
const char *const limit_exception = "Exception: instruction limit exceeded";

/* Every running mode of a bytecode module that the runtime was built with */
std::vector<RunningMode> running_modes = {
    Mode_Interp,
#if WASM_ENABLE_FAST_JIT != 0
    Mode_Fast_JIT,
#endif
#if WASM_ENABLE_JIT != 0
    Mode_LLVM_JIT,
#endif
#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT != 0 \
    && WASM_ENABLE_LAZY_JIT != 0
    Mode_Multi_Tier_JIT,
#endif
};

std::string
running_mode_name(const testing::TestParamInfo<RunningMode> &info)
{
    switch (info.param) {
        case Mode_Interp:
            return "interp";
        case Mode_Fast_JIT:
            return "fast_jit";
        case Mode_LLVM_JIT:
            return "llvm_jit";
        case Mode_Multi_Tier_JIT:
            return "multi_tier_jit";
        default:
            return "unknown";
    }
}

/* Whether the limit is expected to be enforced in a running mode: in the
   interpreter and in the Fast JIT */
bool
expect_enforced(RunningMode running_mode)
{
    return running_mode == Mode_Interp || running_mode == Mode_Fast_JIT;
}

/*
 * The native functions of wasm-apps/nested.wat: they call an export of the
 * module that calls them, on the same exec env.
 */

/* The exception of the last call that call_back_and_swallow swallowed */
std::string swallowed_exception;

bool
call_export(wasm_exec_env_t exec_env, int32_t which, int32_t arg,
            int32_t *result)
{
    static const char *const names[] = { "count", "count_and_trap", "ladder" };
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    wasm_function_inst_t func = nullptr;
    uint32_t argv[1] = { (uint32_t)arg };

    if (which >= 0 && which < 3)
        func = wasm_runtime_lookup_function(module_inst, names[which]);
    if (!func) {
        wasm_runtime_set_exception(module_inst, "no such export");
        return false;
    }
    if (!wasm_runtime_call_wasm(exec_env, func, 1, argv))
        return false;
    *result = (int32_t)argv[0];
    return true;
}

int32_t
call_back(wasm_exec_env_t exec_env, int32_t which, int32_t arg)
{
    int32_t result = 0;

    /* If the call fails, its exception makes the caller fail as well */
    call_export(exec_env, which, arg, &result);
    return result;
}

int32_t
call_back_and_swallow(wasm_exec_env_t exec_env, int32_t which, int32_t arg)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    int32_t result = 0;

    if (!call_export(exec_env, which, arg, &result)) {
        swallowed_exception = wasm_runtime_get_exception(module_inst);
        wasm_runtime_clear_exception(module_inst);
        return -1;
    }
    return result;
}

void
set_limit(wasm_exec_env_t exec_env, int32_t limit)
{
    wasm_runtime_set_instruction_count_limit(exec_env, limit);
}

/* Leave the call that is running only so many instructions to count
   down. A call without a limit counts down from INTPTR_MAX, and this is
   where it gets to by itself in a runtime for a 32-bit target. */
void
run_down(wasm_exec_env_t exec_env, int32_t left)
{
    exec_env->instructions_left = left;
}

/* Leave the call that is running so many instructions to count down, and
   so many more beyond them, which the count is filled up from. This is
   how a limit is kept that a machine word can't hold. */
void
leave(wasm_exec_env_t exec_env, int32_t left, int32_t reserve)
{
    exec_env->instructions_left = left;
    exec_env->instructions_reserve = reserve;
}

/* What the call that is running had left when it called look() */
int64_t seen_left;

void
look(wasm_exec_env_t exec_env)
{
    seen_left =
        (int64_t)exec_env->instructions_left + exec_env->instructions_reserve;
}

/* How often the functions of wasm-apps/init.wat go around their loops:
   the start function, __post_instantiate, __wasm_call_ctors, malloc and
   free */
uint32_t rounds_of[5];
/* Too often to wait for */
const uint32_t without_end = UINT32_MAX;

int32_t
rounds(wasm_exec_env_t exec_env, int32_t which)
{
    (void)exec_env;
    return which >= 0 && which < 5 ? (int32_t)rounds_of[which] : 0;
}

/* Ask the runtime for memory of the module instance and give it back,
   during a call: the runtime runs the malloc and the free that the
   module exports */
int32_t
ask_for_memory(wasm_exec_env_t exec_env, int32_t size)
{
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    uint64_t offset =
        wasm_runtime_module_malloc(module_inst, (uint64_t)size, nullptr);

    if (offset)
        wasm_runtime_module_free(module_inst, offset);
    return (int32_t)offset;
}

NativeSymbol native_symbols[] = {
    { "call_back", (void *)call_back, "(ii)i", nullptr },
    { "call_back_and_swallow", (void *)call_back_and_swallow, "(ii)i",
      nullptr },
    { "set_limit", (void *)set_limit, "(i)", nullptr },
    { "run_down", (void *)run_down, "(i)", nullptr },
    { "leave", (void *)leave, "(ii)", nullptr },
    { "look", (void *)look, "()", nullptr },
    { "rounds", (void *)rounds, "(i)i", nullptr },
    { "ask_for_memory", (void *)ask_for_memory, "(i)i", nullptr },
};

/* How a call ended */
enum Ending { RETURNED, LIMIT_EXCEEDED, TRAPPED };

class InstructionLimitTestBase : public testing::Test
{
  protected:
    void SetUp() override
    {
        memset(&init_args, 0, sizeof(RuntimeInitArgs));
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));
        ASSERT_TRUE(wasm_runtime_register_natives(
            "env", native_symbols,
            sizeof(native_symbols) / sizeof(native_symbols[0])));
        swallowed_exception.clear();
        for (uint32_t &r : rounds_of)
            r = 10;
    }

    void TearDown() override
    {
        deinstantiate();
        if (module)
            wasm_runtime_unload(module);
        wasm_runtime_destroy();
    }

    /* Load a module from a file and make an instance of it */
    void load(const char *file_name)
    {
        std::ifstream file(file_name, std::ios::binary);

        ASSERT_TRUE(file.good()) << "cannot read " << file_name;
        module_buf.assign(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
        module =
            wasm_runtime_load(module_buf.data(), (uint32_t)module_buf.size(),
                              error_buf, sizeof(error_buf));
        ASSERT_NE(module, nullptr) << error_buf;
        instantiate();
    }

    void deinstantiate()
    {
        if (exec_env)
            wasm_runtime_destroy_exec_env(exec_env);
        if (module_inst)
            wasm_runtime_deinstantiate(module_inst);
        exec_env = nullptr;
        module_inst = nullptr;
    }

    /* Make a new instance, in the running mode of the test if it has one */
    void instantiate()
    {
        deinstantiate();
        module_inst = wasm_runtime_instantiate(module, stack_size, 0, error_buf,
                                               sizeof(error_buf));
        ASSERT_NE(module_inst, nullptr) << error_buf;
        if (running_mode != Mode_Default) {
            ASSERT_TRUE(
                wasm_runtime_set_running_mode(module_inst, running_mode));
        }
        exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
        ASSERT_NE(exec_env, nullptr);
    }

    /* Make a new instance whose exec envs start with an instruction
       count limit. It is in the running mode of the test from the
       beginning, where the runtime runs functions of the module. */
    bool instantiate_with_limit(int64_t limit)
    {
        struct InstantiationArgs2 *args = nullptr;

        deinstantiate();
        if (running_mode != Mode_Default)
            EXPECT_TRUE(wasm_runtime_set_default_running_mode(running_mode));
        EXPECT_TRUE(wasm_runtime_instantiation_args_create(&args));
        if (!args)
            return false;
        wasm_runtime_instantiation_args_set_default_stack_size(args,
                                                               stack_size);
        wasm_runtime_instantiation_args_set_instruction_count_limit(args,
                                                                    limit);
        module_inst = wasm_runtime_instantiate_ex2(module, args, error_buf,
                                                   sizeof(error_buf));
        wasm_runtime_instantiation_args_destroy(args);
        if (!module_inst)
            return false;
        exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
        return exec_env != nullptr;
    }

    bool enforced()
    {
        return wasm_runtime_is_instruction_count_limit_enforced(module_inst);
    }

    /* Call an export without touching the limit */
    Ending call(const char *name, std::vector<uint32_t> args,
                uint32_t *result = nullptr)
    {
        wasm_function_inst_t func =
            wasm_runtime_lookup_function(module_inst, name);

        EXPECT_NE(func, nullptr) << name;
        if (!func)
            return TRAPPED;
        /* room for the result */
        args.resize(args.size() + 2);
        wasm_runtime_clear_exception(module_inst);
        if (wasm_runtime_call_wasm(exec_env, func, (uint32_t)args.size() - 2,
                                   args.data())) {
            if (result)
                *result = args[0];
            return RETURNED;
        }
        exception = wasm_runtime_get_exception(module_inst);
        return exception == limit_exception ? LIMIT_EXCEEDED : TRAPPED;
    }

    /* Call an export under a limit */
    Ending call(const char *name, std::vector<uint32_t> args, int limit,
                uint32_t *result)
    {
        wasm_runtime_set_instruction_count_limit(exec_env, limit);
        return call(name, args, result);
    }

    /* Call an export under a limit that needn't fit into an int */
    Ending call64(const char *name, std::vector<uint32_t> args, int64_t limit,
                  uint32_t *result)
    {
        wasm_runtime_set_instruction_count_limit64(exec_env, limit);
        return call(name, args, result);
    }

    /* Call an export of a new instance under a limit */
    Ending call_new(const char *name, const std::vector<uint32_t> &args,
                    int limit, uint32_t *result = nullptr)
    {
        instantiate();
        if (!exec_env)
            return TRAPPED;
        return call(name, args, limit, result);
    }

    /* The cost of a call: the least limit that the call doesn't reach,
       found by trying */
    int cost(const char *name, const std::vector<uint32_t> &args)
    {
        int low = 0, high = 1;

        if (call_new(name, args, 0) != LIMIT_EXCEEDED)
            return 0;
        while (call_new(name, args, high) == LIMIT_EXCEEDED) {
            low = high;
            if (high > INT_MAX / 2) {
                ADD_FAILURE() << name << " doesn't end under any limit";
                return INT_MAX;
            }
            high *= 2;
        }
        /* low is reached, high isn't */
        while (high - low > 1) {
            int middle = low + (high - low) / 2;

            if (call_new(name, args, middle) == LIMIT_EXCEEDED)
                low = middle;
            else
                high = middle;
        }
        return high;
    }

  public:
    RuntimeInitArgs init_args;
    char error_buf[256];
    std::vector<uint8_t> module_buf;
    RunningMode running_mode = static_cast<RunningMode>(Mode_Default);
    uint32_t stack_size = 64 * 1024;
    wasm_module_t module = nullptr;
    wasm_module_inst_t module_inst = nullptr;
    wasm_exec_env_t exec_env = nullptr;
    std::string exception;
};

/* The tests which run in every running mode */
class InstructionLimitTest
  : public InstructionLimitTestBase
  , public testing::WithParamInterface<RunningMode>
{
  protected:
    void SetUp() override
    {
        running_mode = GetParam();
        InstructionLimitTestBase::SetUp();
    }
};

#define LOAD(file_name) ASSERT_NO_FATAL_FAILURE(load(file_name))

/* For the tests of a limit that is enforced */
#define LOAD_ENFORCED(file_name)                                         \
    LOAD(file_name);                                                     \
    if (!enforced()) {                                                   \
        GTEST_SKIP() << "the instruction count limit isn't enforced in " \
                        "this running mode";                             \
    }                                                                    \
    (void)0

/*
 * Where the limit is enforced
 */

TEST_P(InstructionLimitTest, enforced_as_documented)
{
    LOAD("control.wasm");
    EXPECT_EQ(wasm_runtime_get_running_mode(module_inst), GetParam());
    EXPECT_EQ(enforced(), expect_enforced(GetParam()));
}

TEST_F(InstructionLimitTestBase, enforced_as_documented_in_the_default_mode)
{
    LOAD("control.wasm");
    EXPECT_EQ(enforced(),
              expect_enforced(wasm_runtime_get_running_mode(module_inst)));
    /* The interpreter is in every runtime that runs bytecode modules */
    ASSERT_TRUE(wasm_runtime_set_running_mode(module_inst, Mode_Interp));
    EXPECT_TRUE(enforced());
}

#if WASM_ENABLE_JIT != 0
/* The LLVM JIT doesn't count: this is what a host has to find out */
TEST_F(InstructionLimitTestBase, ignored_by_the_llvm_jit)
{
    uint32_t result = 0;

    running_mode = Mode_LLVM_JIT;
    LOAD("control.wasm");
    EXPECT_FALSE(enforced());
    EXPECT_EQ(call("loop_br", { 100000 }, 10, &result), RETURNED);
    EXPECT_EQ(result, 100000u);
}
#endif

#if WASM_ENABLE_JIT != 0 && WASM_ENABLE_FAST_JIT != 0 \
    && WASM_ENABLE_LAZY_JIT != 0
/* In Multi-tier JIT mode a function runs the code of Fast JIT, which
   counts, until its code of LLVM JIT is ready, which doesn't. The limit
   is ignored all the time, not from some call on. */
TEST_F(InstructionLimitTestBase, ignored_in_multi_tier_jit_mode)
{
    uint32_t result = 0;

    running_mode = Mode_Multi_Tier_JIT;
    LOAD("control.wasm");
    EXPECT_FALSE(enforced());
    for (int i = 0; i < 20; i++) {
        SCOPED_TRACE(i);
        EXPECT_EQ(call("loop_br", { 100000 }, 10, &result), RETURNED);
        EXPECT_EQ(result, 100000u);
    }
}

/* A module instance in Fast JIT mode runs the code of Fast JIT, which
   counts, also when the function has tiered up to LLVM JIT for the
   instances in Multi-tier JIT mode, and also when it is called through
   the stub that compiles a function at its first call */
TEST_F(InstructionLimitTestBase, fast_jit_mode_stays_out_of_llvm_jit_code)
{
    uint32_t result = 0;

    /* LLVM JIT compiles the functions of a module that has an instance
       in a mode which runs them */
    running_mode = Mode_Multi_Tier_JIT;
    LOAD("control.wasm");

    /* Call the function until it has tiered up: the code of LLVM JIT
       doesn't count */
    for (int tries = 0;; tries++) {
        ASSERT_EQ(call("loop_br", { 10 }, -1, &result), RETURNED);
        if (exec_env->instructions_left == INTPTR_MAX)
            break;
        if (tries == 6000) {
            GTEST_SKIP() << "the function didn't tier up within a minute";
        }
        os_usleep(10000);
    }

    ASSERT_TRUE(wasm_runtime_set_running_mode(module_inst, Mode_Fast_JIT));
    EXPECT_TRUE(enforced());
    EXPECT_EQ(call("loop_br", { 100000 }, 10, &result), LIMIT_EXCEEDED);

    /* A call that found the function not compiled yet goes through the
       stub, which may get its turn only after the function has tiered
       up. Put the stub back into the table of the instance to make
       such a call. */
    WASMModuleInstance *inst = (WASMModuleInstance *)module_inst;
    WASMFunctionInstance *func =
        (WASMFunctionInstance *)wasm_runtime_lookup_function(module_inst,
                                                             "loop_br");
    ASSERT_NE(func, nullptr);
    uint32 func_idx = (uint32)(func - inst->e->functions);
    inst->fast_jit_func_ptrs[func_idx - inst->module->import_function_count] =
        jit_compiler_get_jit_globals()->compile_fast_jit_and_then_call;
    EXPECT_EQ(call("loop_br", { 100000 }, 10, &result), LIMIT_EXCEEDED);
}
#endif

/*
 * What is counted
 */

struct ControlCase {
    const char *name;
    uint32_t arg;
    uint32_t result;
    /* the opcodes which the classic interpreter dispatches */
    int count;
};

/* clang-format off */
const ControlCase control_cases[] = {
    { "straight", 5, 18, 7 },
    { "block", 5, 5, 4 },
    { "block_in_block", 5, 5, 8 },
    { "loop_once", 5, 5, 4 },
    /* local.get, if; local.get, end */
    { "if", 0, 0, 4 },
    /* local.get, if; i32.const, local.set, end; local.get, end */
    { "if", 1, 5, 7 },
    /* local.get, if; i32.const, end; end */
    { "if_else", 0, 2, 5 },
    /* local.get, if; i32.const, else; end; end */
    { "if_else", 1, 1, 6 },
    { "if_true", 4, 4, 7 },
    { "if_false", 4, 0, 4 },
    { "if_else_true", 4, 4, 6 },
    { "if_else_false", 4, 4, 5 },
    { "if_in_if", 0, 0, 12 },
    { "if_in_if", 1, 1, 13 },
    { "if_in_if", 2, 2, 13 },
    { "if_in_if", 3, 3, 14 },
    { "if_then_if", 0, 0, 6 },
    { "if_then_if", 1, 1, 13 },
    { "if_then_if", 3, 3, 16 },
    /* block, local.get, br; end; end */
    { "br", 5, 5, 5 },
    /* the end of the block that is branched to, not of those inside it */
    { "br_outer", 5, 5, 7 },
    { "br_if", 0, 6, 8 },
    { "br_if", 1, 5, 6 },
    { "br_if_value", 0, 106, 10 },
    { "br_if_value", 1, 105, 8 },
    { "br_if_twice", 0, 10, 18 },
    { "br_if_twice", 1, 0, 8 },
    { "br_if_twice", 2, 0, 12 },
    { "br_if_twice", 3, 7, 20 },
    { "br_if_twice", 4, 18, 27 },
    { "br_table", 0, 10, 11 },
    { "br_table", 1, 20, 11 },
    { "br_table", 2, 30, 10 },
    { "br_table", 9, 30, 10 },
    /* 8 opcodes for each time around, and loop; end, local.get, end */
    { "loop_br_if", 1, 1, 12 },
    { "loop_br_if", 10, 10, 84 },
    { "loop_br", 0, 0, 9 },
    { "loop_br", 10, 10, 99 },
    { "loop_br_table", 1, 1, 13 },
    { "loop_br_table", 10, 10, 85 },
    { "loop_if", 1, 1, 12 },
    { "loop_if", 10, 10, 93 },
    { "loop_in_loop", 1, (uint32_t)-1, 33 },
    { "loop_in_loop", 6, 0, 354 },
    { "loop_param", 1, 3, 14 },
    { "loop_param", 10, 30, 104 },
    { "return", 0, 2, 4 },
    { "return", 1, 1, 4 },
    { "br_func", 0, 4, 6 },
    { "br_func", 1, 3, 4 },
    { "dead", 5, 5, 5 },
    { "dead_in_if", 0, 3, 5 },
    { "dead_in_if", 1, 1, 4 },
    /* local.get, call; local.get, i32.const, i32.add, end; end */
    { "call", 5, 6, 7 },
    { "call_in_call", 5, 6, 17 },
    { "call_indirect", 4, 5, 10 },
    { "call_indirect", 5, 4, 10 },
    { "recursion", 1, 1, 8 },
    { "recursion", 6, 720, 68 },
    { "loop_call", 1, 1, 15 },
    { "loop_call", 10, 10, 114 },
    { "multi_value", 0, 8, 16 },
    { "multi_value", 5, 15, 17 },
    { "memory", 1, 1, 34 },
    { "memory", 10, 2, 151 },
    { "trap", 5, 21, 6 },
    /* local.get, end */
    { "few_locals", 5, 5, 2 },
    /* and five for its local variables */
    { "many_locals", 5, 5, 7 },
};
/* clang-format on */

/* A call returns under a limit if and only if it doesn't count more
   instructions than that, and returns the same then as without a limit */
TEST_P(InstructionLimitTest, control_constructs)
{
    LOAD_ENFORCED("control.wasm");

    for (const ControlCase &c : control_cases) {
        SCOPED_TRACE(std::string(c.name) + " " + std::to_string(c.arg));
        uint32_t result = 0;
        int count = cost(c.name, { c.arg });

#if WASM_ENABLE_FAST_INTERP == 0
        /* The classic interpreter counts every opcode it dispatches, and
           the Fast JIT counts the same */
        EXPECT_EQ(count, c.count);
#else
        /* The fast interpreter counts the operations which the loader
           turned the opcodes into: there are fewer of them */
        EXPECT_GT(count, 0);
        EXPECT_LE(count, c.count);
#endif
        EXPECT_EQ(call_new(c.name, { c.arg }, count - 1), LIMIT_EXCEEDED);
        EXPECT_EQ(call_new(c.name, { c.arg }, count, &result), RETURNED);
        EXPECT_EQ(result, c.result);
        EXPECT_EQ(call_new(c.name, { c.arg }, -1, &result), RETURNED);
        EXPECT_EQ(result, c.result);
    }
}

/* A call that traps: the limit ends it if it is reached before the trap */
TEST_P(InstructionLimitTest, trap)
{
    LOAD_ENFORCED("control.wasm");

    /* i32.const, local.get, i32.div_u (which traps), i32.const, i32.add,
       end */
    int count = cost("trap", { 0 });

    EXPECT_EQ(call_new("trap", { 0 }, count), TRAPPED);
    EXPECT_EQ(exception, "Exception: integer divide by zero");
    EXPECT_EQ(call_new("trap", { 0 }, -1), TRAPPED);
    EXPECT_EQ(call_new("trap", { 0 }, count - 1), LIMIT_EXCEEDED);
#if WASM_ENABLE_FAST_INTERP == 0
    if (GetParam() == Mode_Interp) {
        EXPECT_EQ(count, 3);
    }
    else {
        /* The jitted code pays for the opcodes that are run together
           before it runs them, so it may want more than those before
           the trap, but not more than all of them */
        EXPECT_GE(count, 3);
        EXPECT_LE(count, 6);
    }
#endif
}

#if WASM_ENABLE_FAST_JIT != 0
/* The Fast JIT counts what the classic interpreter counts, for every
   function of control.wat that returns */
TEST_F(InstructionLimitTestBase, fast_jit_counts_like_the_interpreter)
{
    static const char *const functions[] = {
        "straight",
        "block",
        "block_in_block",
        "loop_once",
        "if",
        "if_else",
        "if_true",
        "if_false",
        "if_else_true",
        "if_else_false",
        "if_in_if",
        "if_then_if",
        "br",
        "br_outer",
        "br_if",
        "br_if_value",
        "br_if_twice",
        "br_table",
        "loop_br_if",
        "loop_br",
        "loop_br_table",
        "loop_if",
        "loop_in_loop",
        "loop_param",
        "return",
        "br_func",
        "dead",
        "dead_in_if",
        "call",
        "call_in_call",
        "call_indirect",
        "recursion",
        "loop_call",
        "multi_value",
        "memory",
        "few_locals",
        "many_locals",
        "loop_call_few_locals",
        "loop_call_many_locals",
    };

    LOAD("control.wasm");

    for (const char *name : functions) {
        for (uint32_t arg = 0; arg < 12; arg++) {
            SCOPED_TRACE(std::string(name) + " " + std::to_string(arg));
            uint32_t result_interp = 0, result_jit = 1;

            running_mode = Mode_Interp;
            int count_interp = cost(name, { arg });
            EXPECT_EQ(call_new(name, { arg }, count_interp, &result_interp),
                      RETURNED);

            running_mode = Mode_Fast_JIT;
            int count_jit = cost(name, { arg });
            EXPECT_EQ(call_new(name, { arg }, count_jit, &result_jit),
                      RETURNED);

            EXPECT_EQ(count_jit, count_interp);
            EXPECT_EQ(result_jit, result_interp);
        }
    }
}
#endif

/* A call counts one more for every 64 bytes of the local variables of
   the function, which it sets to zero */
TEST_P(InstructionLimitTest, calls_count_the_local_variables)
{
    LOAD_ENFORCED("control.wasm");

    /* 320 bytes of local variables against 56 */
    EXPECT_EQ(cost("many_locals", { 5 }) - cost("few_locals", { 5 }), 5);
    for (uint32_t n : { 1u, 10u, 100u }) {
        SCOPED_TRACE(n);
        EXPECT_EQ(cost("loop_call_many_locals", { n })
                      - cost("loop_call_few_locals", { n }),
                  5 * (int)n);
    }
}

/* An instruction that handles a block of memory counts one more for every
   64 bytes of it, and one that handles table elements one more for every
   8 of them */
TEST_P(InstructionLimitTest, bulk_instructions_count_what_they_handle)
{
    static const struct {
        const char *name;
        /* what is handled for one more instruction */
        uint32_t unit;
        /* what there is to handle */
        uint32_t size;
    } functions[] = {
        { "memory_fill", 64, 262144 }, { "memory_copy", 64, 131072 },
        { "memory_init", 64, 256 },    { "table_fill", 8, 2040 },
        { "table_copy", 8, 1024 },     { "table_init", 8, 64 },
    };

    LOAD_ENFORCED("bulk.wasm");

    for (const auto &f : functions) {
        int count = cost(f.name, { 0 });

        for (uint32_t n : { f.unit - 1, f.unit, f.unit + 1, f.size }) {
            SCOPED_TRACE(std::string(f.name) + " " + std::to_string(n));
            EXPECT_EQ(cost(f.name, { n }), count + (int)(n / f.unit));
        }
    }
}

#if WASM_ENABLE_FAST_JIT != 0
TEST_F(InstructionLimitTestBase, fast_jit_counts_bulk_like_the_interpreter)
{
    static const char *const functions[] = {
        "memory_fill", "memory_copy", "memory_init",
        "table_fill",  "table_copy",  "table_init",
    };

    LOAD("bulk.wasm");

    for (const char *name : functions) {
        /* table.init has 64 elements to take */
        for (uint32_t n : { 0u, 1u, 8u, 63u, 64u }) {
            SCOPED_TRACE(std::string(name) + " " + std::to_string(n));
            uint32_t result_interp = 0, result_jit = 1;

            running_mode = Mode_Interp;
            int count_interp = cost(name, { n });
            EXPECT_EQ(call_new(name, { n }, count_interp, &result_interp),
                      RETURNED);

            running_mode = Mode_Fast_JIT;
            int count_jit = cost(name, { n });
            EXPECT_EQ(call_new(name, { n }, count_jit, &result_jit), RETURNED);

            EXPECT_EQ(count_jit, count_interp);
            EXPECT_EQ(result_jit, result_interp);
        }
    }
}
#endif

/* So a loop of such instructions is stopped after as many bytes as the
   limit allows, not after as many times around */
TEST_P(InstructionLimitTest, bulk_instructions_without_end)
{
    LOAD_ENFORCED("bulk.wasm");

    /* The first fill of the whole memory takes 4096 instructions */
    EXPECT_EQ(call_new("memory_fill_without_end", {}, 4000, nullptr),
              LIMIT_EXCEEDED);
    EXPECT_EQ(call_new("memory_fill_without_end", {}, 1000000, nullptr),
              LIMIT_EXCEEDED);
    /* and the limit is told from a trap: there are only 256 bytes of
       data */
    EXPECT_EQ(call_new("memory_init", { 257 }, 1000000, nullptr), TRAPPED);
    EXPECT_EQ(exception, "Exception: out of bounds memory access");
    /* A call without a limit isn't stopped */
    uint32_t result = 0;
    EXPECT_EQ(call_new("memory_fill", { 262144 }, -1, &result), RETURNED);
    EXPECT_EQ(result, 7u);
}

/*
 * What the limit stops
 */

TEST_P(InstructionLimitTest, loops_and_recursions_without_end)
{
    static const struct {
        const char *name;
        uint32_t argc;
    } functions[] = {
        { "loop_br", 0 },
        { "loop_br_if", 0 },
        { "loop_br_if_local", 1 },
        { "loop_br_table", 0 },
        { "loop_br_table_local", 1 },
        { "loop_if", 0 },
        { "loop_if_local", 1 },
        { "loop_if_else", 1 },
        { "loop_block", 0 },
        { "loop_in_loop", 1 },
        { "loop_param", 0 },
        { "loop_call", 0 },
        { "loop_call_indirect", 0 },
        /* these end with a stack overflow if the limit is high enough */
        { "recursion", 0 },
        { "recursion_indirect", 0 },
        { "recursion_mutual", 0 },
    };

    LOAD_ENFORCED("forever.wasm");

    for (const auto &f : functions) {
        for (int limit : { 0, 1, 2, 3, 10, 100 }) {
            SCOPED_TRACE(std::string(f.name) + " " + std::to_string(limit));
            EXPECT_EQ(call_new(f.name, std::vector<uint32_t>(f.argc, 0), limit,
                               nullptr),
                      LIMIT_EXCEEDED);
        }
    }
    /* and the loops under a high one */
    for (const auto &f : functions) {
        if (!strncmp(f.name, "loop", 4)) {
            SCOPED_TRACE(f.name);
            EXPECT_EQ(call_new(f.name, std::vector<uint32_t>(f.argc, 0),
                               1000000, nullptr),
                      LIMIT_EXCEEDED);
        }
    }
}

/*
 * The values of the limit
 */

TEST_P(InstructionLimitTest, limit_values)
{
    uint32_t result = 0;

    LOAD_ENFORCED("control.wasm");

    /* No limit unless one is set */
    EXPECT_EQ(call("loop_br", { 100000 }, &result), RETURNED);
    EXPECT_EQ(result, 100000u);

    /* Nothing runs under a limit of 0 */
    EXPECT_EQ(call("straight", { 5 }, 0, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call("loop_br", { 100000 }, 0, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call("loop_br", { 100000 }, 1, &result), LIMIT_EXCEEDED);

    /* A negative limit is no limit */
    EXPECT_EQ(call("loop_br", { 100000 }, -1, &result), RETURNED);
    EXPECT_EQ(result, 100000u);
    EXPECT_EQ(call("loop_br", { 100000 }, INT_MIN, &result), RETURNED);
    EXPECT_EQ(result, 100000u);

    EXPECT_EQ(call("loop_br", { 100000 }, INT_MAX, &result), RETURNED);
    EXPECT_EQ(result, 100000u);
}

/* A limit that doesn't fit into an int */
TEST_P(InstructionLimitTest, limit_of_64_bits)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("count", { 1000 });

    instantiate();
    EXPECT_EQ(call64("count", { 1000 }, count - 1, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call64("count", { 1000 }, count, &result), RETURNED);
    EXPECT_EQ(result, 1000u);

    /* It isn't cut down to an int, which would leave nothing of the first
       of them and not enough of the second */
    for (int64_t limit : { (int64_t)1 << 32, ((int64_t)1 << 32) + count - 1,
                           (int64_t)INT_MAX + 1, INT64_MAX }) {
        SCOPED_TRACE(limit);
        EXPECT_EQ(call64("count", { 1000 }, limit, &result), RETURNED);
        EXPECT_EQ(result, 1000u);
    }

    /* A negative limit is no limit */
    for (int64_t limit : { (int64_t)-1, -((int64_t)1 << 32), INT64_MIN }) {
        SCOPED_TRACE(limit);
        EXPECT_EQ(call64("count", { 1000 }, limit, &result), RETURNED);
        EXPECT_EQ(result, 1000u);
    }

    /* The call has all of the limit, less the few instructions before
       the native function looks */
    int64_t limit = ((int64_t)1 << 40) + 12345;

    seen_left = 0;
    EXPECT_EQ(call64("look_and_count", { 1000 }, limit, &result), RETURNED);
    EXPECT_LT(seen_left, limit);
    EXPECT_GT(seen_left, limit - 100);
}

/* The count is a machine word, which is filled up from what the call has
   left beyond it. The code of the Fast JIT doesn't do that: it is for
   targets with 64-bit words only, which hold all of a limit. */
TEST_P(InstructionLimitTest, count_is_filled_up_from_what_is_left)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");
    if (GetParam() != Mode_Interp) {
        GTEST_SKIP() << "only the interpreters fill up the count";
    }

    /* What counting to 1000 takes after the native function */
    uint32_t low = 0, high = 1000000;

    ASSERT_EQ(call("leave_and_count", { low, 0, 1000 }, 1000000, &result),
              LIMIT_EXCEEDED);
    ASSERT_EQ(call("leave_and_count", { high, 0, 1000 }, 1000000, &result),
              RETURNED);
    while (high - low > 1) {
        uint32_t middle = low + (high - low) / 2;

        if (call("leave_and_count", { middle, 0, 1000 }, 1000000, &result)
            == LIMIT_EXCEEDED)
            low = middle;
        else
            high = middle;
    }

    /* It takes the same when a part of it is left beyond the count */
    for (uint32_t left : { 0u, 1u, high / 2, high - 1 }) {
        SCOPED_TRACE(left);
        EXPECT_EQ(call("leave_and_count", { left, high - left, 1000 }, 1000000,
                       &result),
                  RETURNED);
        EXPECT_EQ(result, 1000u);
        EXPECT_EQ(call("leave_and_count", { left, high - left - 1, 1000 },
                       1000000, &result),
                  LIMIT_EXCEEDED);
    }
}

/* Each call that the host makes has the whole limit, also after a call
   that reached it */
TEST_P(InstructionLimitTest, every_call_has_the_whole_limit)
{
    uint32_t result = 0;

    LOAD_ENFORCED("control.wasm");

    int count = cost("loop_br", { 100 });

    instantiate();
    wasm_runtime_set_instruction_count_limit(exec_env, count);
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(call("loop_br", { 100 }, &result), RETURNED);
        EXPECT_EQ(result, 100u);
    }
    EXPECT_EQ(call("loop_br", { 101 }, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call("loop_br", { 1000000 }, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call("loop_br", { 100 }, &result), RETURNED);
    EXPECT_EQ(result, 100u);
}

/*
 * What the runtime runs by itself
 */

/* The functions which the runtime runs when it instantiates a module run
   under the limit that the instance is given */
TEST_P(InstructionLimitTest, functions_run_at_instantiation)
{
    uint32_t result = 0;

    LOAD_ENFORCED("init.wasm");

    /* the start function, __post_instantiate, __wasm_call_ctors */
    for (int which : { 0, 1, 2 }) {
        SCOPED_TRACE(which);
        rounds_of[which] = without_end;
        EXPECT_FALSE(instantiate_with_limit(100000));
        EXPECT_NE(std::string(error_buf).find("instruction limit exceeded"),
                  std::string::npos)
            << error_buf;
        rounds_of[which] = 10;
    }

    /* Each of them has the whole limit */
    for (uint32_t &r : rounds_of)
        r = 1000;
    ASSERT_TRUE(instantiate_with_limit(-1)) << error_buf;
    int count = cost("count", { 1000 });
    EXPECT_FALSE(instantiate_with_limit(count / 2));
    ASSERT_TRUE(instantiate_with_limit(count + 100)) << error_buf;
    EXPECT_EQ(call("ran", {}, -1, &result), RETURNED);
    EXPECT_EQ(result, 7u);
}

/* The malloc and free of a module, which the runtime runs for the host
   with an exec env of its own when no call into the module is running,
   run under the limit that the instance was given */
TEST_P(InstructionLimitTest, malloc_and_free_of_the_module)
{
    uint32_t result = 0;
    void *native_addr = nullptr;

    LOAD_ENFORCED("init.wasm");

    ASSERT_TRUE(instantiate_with_limit(100000)) << error_buf;

    EXPECT_EQ(wasm_runtime_module_malloc(module_inst, 16, &native_addr), 1024u);
    wasm_runtime_module_free(module_inst, 1024);
    EXPECT_EQ(wasm_runtime_get_exception(module_inst), nullptr);
    EXPECT_EQ(call("ran", {}, -1, &result), RETURNED);
    EXPECT_EQ(result, 31u);

    rounds_of[3] = without_end;
    EXPECT_EQ(wasm_runtime_module_malloc(module_inst, 16, &native_addr), 0u);
    EXPECT_STREQ(wasm_runtime_get_exception(module_inst), limit_exception);
    wasm_runtime_clear_exception(module_inst);

    rounds_of[4] = without_end;
    wasm_runtime_module_free(module_inst, 1024);
    EXPECT_STREQ(wasm_runtime_get_exception(module_inst), limit_exception);
    wasm_runtime_clear_exception(module_inst);
}

/* When a native function asks for them during a call, they count against
   the limit of that call */
TEST_P(InstructionLimitTest, malloc_and_free_of_the_module_in_a_call)
{
    uint32_t result = 0;

    LOAD_ENFORCED("init.wasm");

    /* What malloc and free count is counted */
    int count = cost("malloc_by_native", { 16 });
    rounds_of[3] = 1010;
    int count_malloc = cost("malloc_by_native", { 16 });
    rounds_of[4] = 1010;
    int count_free = cost("malloc_by_native", { 16 });

    EXPECT_GE(count_malloc - count, 1000);
    EXPECT_EQ(count_free - count_malloc, count_malloc - count);
    EXPECT_EQ(call_new("malloc_by_native", { 16 }, count_free, &result),
              RETURNED);
    EXPECT_EQ(result, 1024u);

    /* and what they don't get to the end of is stopped */
    rounds_of[4] = 10;
    rounds_of[3] = without_end;
    EXPECT_EQ(call_new("malloc_by_native", { 16 }, 100000, &result),
              LIMIT_EXCEEDED);
    rounds_of[3] = 10;
    rounds_of[4] = without_end;
    EXPECT_EQ(call_new("malloc_by_native", { 16 }, 100000, &result),
              LIMIT_EXCEEDED);
}

/* An exec env starts with the limit that the instance was given */
TEST_P(InstructionLimitTest, exec_env_starts_with_the_limit_of_the_instance)
{
    uint32_t result = 0;

    LOAD_ENFORCED("init.wasm");

    ASSERT_TRUE(instantiate_with_limit(1000)) << error_buf;
    EXPECT_EQ(call("count", { 100000 }, &result), LIMIT_EXCEEDED);
    EXPECT_EQ(call("count", { 10 }, &result), RETURNED);
    EXPECT_EQ(result, 10u);

    /* until it is given one of its own */
    EXPECT_EQ(call("count", { 100000 }, -1, &result), RETURNED);
    EXPECT_EQ(result, 100000u);

    /* and it starts with none if the instance was given none */
    instantiate();
    EXPECT_EQ(call("count", { 100000 }, &result), RETURNED);
    EXPECT_EQ(result, 100000u);
}

/*
 * Calls nested through native functions
 */

/* A call that a native function makes into the module counts against the
   limit of the call that the native function is in */
TEST_P(InstructionLimitTest, nested_calls_share_the_limit)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("count", { 500 });
    int once = cost("call_count", { 1, 500 });
    int twice = cost("call_count", { 2, 500 });
    int thrice = cost("call_count", { 3, 500 });

    /* Every nested call costs what it counts, and a bit */
    EXPECT_GT(once, count);
    EXPECT_GT(twice - once, count);
    EXPECT_EQ(thrice - twice, twice - once);

    /* What is enough for one nested call isn't enough for two */
    EXPECT_EQ(call_new("call_count", { 2, 500 }, once, &result),
              LIMIT_EXCEEDED);
    EXPECT_EQ(call_new("call_count", { 2, 500 }, twice, &result), RETURNED);
    EXPECT_EQ(result, 1000u);
}

/* The limit is reached below several native functions */
TEST_P(InstructionLimitTest, limit_reached_in_a_nested_call)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("ladder", { 5 });

    EXPECT_GT(count, cost("count", { 100 }));
    for (int limit : { 1, count / 4, count / 2, count - 1 }) {
        SCOPED_TRACE(limit);
        EXPECT_EQ(call_new("ladder", { 5 }, limit, &result), LIMIT_EXCEEDED);
        /* The instance and the exec env are fine afterwards */
        EXPECT_EQ(call("ladder", { 5 }, count, &result), RETURNED);
        EXPECT_EQ(result, 100u);
    }
}

/* The limit is reached below several functions of the module */
TEST_P(InstructionLimitTest, limit_reached_deep_in_the_call_stack)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("deep", { 20, 1000 });

    EXPECT_GT(count, cost("count", { 1000 }));
    for (int limit : { count / 2, count - 1 }) {
        SCOPED_TRACE(limit);
        EXPECT_EQ(call_new("deep", { 20, 1000 }, limit, &result),
                  LIMIT_EXCEEDED);
        EXPECT_EQ(call("deep", { 20, 1000 }, count, &result), RETURNED);
        EXPECT_EQ(result, 1000u);
    }
}

/* When a native function swallows the failure of a nested call that
   reached the limit, the call it is in ends there all the same */
TEST_P(InstructionLimitTest, limit_exception_can_not_be_swallowed)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("count", { 500 });

    EXPECT_EQ(call_new("swallow", { 0, 3, 500 }, count / 2, &result),
              LIMIT_EXCEEDED);
    EXPECT_EQ(swallowed_exception, limit_exception);
    /* Nothing ran after the native function returned */
    EXPECT_EQ(call("returned", {}, -1, &result), RETURNED);
    EXPECT_EQ(result, 0u);

    /* The second nested call reaches the limit */
    swallowed_exception.clear();
    EXPECT_EQ(call_new("swallow", { 0, 3, 500 }, count * 3 / 2, &result),
              LIMIT_EXCEEDED);
    EXPECT_EQ(swallowed_exception, limit_exception);
    EXPECT_EQ(call("returned", {}, -1, &result), RETURNED);
    EXPECT_EQ(result, 1u);
}

/* A nested call that traps has run instructions too */
TEST_P(InstructionLimitTest, trap_in_a_nested_call_is_counted)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    int count = cost("count", { 500 });
    int once = cost("swallow", { 1, 1, 500 });
    int twice = cost("swallow", { 1, 2, 500 });

    EXPECT_GT(once, count);
    EXPECT_GT(twice - once, count);

    EXPECT_EQ(call_new("swallow", { 1, 2, 500 }, twice, &result), RETURNED);
    EXPECT_EQ(result, (uint32_t)-2);
    EXPECT_EQ(swallowed_exception, "Exception: out of bounds memory access");
    EXPECT_EQ(call_new("swallow", { 1, 2, 500 }, once, &result),
              LIMIT_EXCEEDED);
}

/* A limit that is set during a call is for the next call of the host */
TEST_P(InstructionLimitTest, limit_set_during_a_call)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");

    EXPECT_EQ(call("set_limit_and_count", { 5, 1000 }, 1000000, &result),
              RETURNED);
    EXPECT_EQ(result, 1000u);
    EXPECT_EQ(call("count", { 1000 }, &result), LIMIT_EXCEEDED);
}

/* When the count of a call without a limit has run out, the interpreters
   start it again. The code of the Fast JIT doesn't: it is for targets
   with 64-bit words only, where no call gets to the end of the count. */
TEST_P(InstructionLimitTest, no_limit_outlasts_the_count)
{
    uint32_t result = 0;

    LOAD_ENFORCED("nested.wasm");
    if (GetParam() != Mode_Interp) {
        GTEST_SKIP() << "only the interpreters start the count again";
    }

    for (uint32_t left : { 0u, 1u, 10u, 1000u }) {
        SCOPED_TRACE(left);
        EXPECT_EQ(call("run_down_and_count", { left, 1000 }, -1, &result),
                  RETURNED);
        EXPECT_EQ(result, 1000u);
    }
    /* With a limit, what is left is all there is */
    EXPECT_EQ(call("run_down_and_count", { 10, 1000 }, 1000000, &result),
              LIMIT_EXCEEDED);
    EXPECT_EQ(call("run_down_and_count", { 100000, 1000 }, 1000000, &result),
              RETURNED);
    EXPECT_EQ(result, 1000u);
}

INSTANTIATE_TEST_SUITE_P(RunningModes, InstructionLimitTest,
                         testing::ValuesIn(running_modes), running_mode_name);

} /* namespace */
