/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "wasm_runtime_common.h"
#include "bh_platform.h"

namespace {

/* What a call ends with when it reaches the limit */
const char *const limit_exception = "Exception: instruction limit exceeded";

/* The running modes that enforce the limit */
std::vector<RunningMode> running_modes = {
    Mode_Interp,
#if WASM_ENABLE_FAST_JIT != 0
    Mode_Fast_JIT,
#endif
};

std::string
running_mode_name(const testing::TestParamInfo<RunningMode> &info)
{
    return info.param == Mode_Interp ? "interp" : "fast_jit";
}

class InstructionLimitThreadTest : public testing::TestWithParam<RunningMode>
{
  protected:
    void SetUp() override
    {
        std::ifstream file("thread.wasm", std::ios::binary);

        memset(&init_args, 0, sizeof(RuntimeInitArgs));
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        /* room for the chain of threads, which gets as long as there are
           bits in the limit */
        init_args.max_thread_num = 64;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));

        ASSERT_TRUE(file.good()) << "cannot read thread.wasm";
        module_buf.assign(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
        module =
            wasm_runtime_load(module_buf.data(), (uint32_t)module_buf.size(),
                              error_buf, sizeof(error_buf));
        ASSERT_NE(module, nullptr) << error_buf;
        module_inst = wasm_runtime_instantiate(module, stack_size, 0, error_buf,
                                               sizeof(error_buf));
        ASSERT_NE(module_inst, nullptr) << error_buf;
        ASSERT_TRUE(wasm_runtime_set_running_mode(module_inst, GetParam()));
        ASSERT_TRUE(
            wasm_runtime_is_instruction_count_limit_enforced(module_inst));
        exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
        ASSERT_NE(exec_env, nullptr);
    }

    void TearDown() override
    {
        /* This waits for the threads, so make them end */
        if (module_inst)
            wasm_runtime_terminate(module_inst);
        if (exec_env)
            wasm_runtime_destroy_exec_env(exec_env);
        if (module_inst)
            wasm_runtime_deinstantiate(module_inst);
        if (module)
            wasm_runtime_unload(module);
        wasm_runtime_destroy();
    }

    /* Call an export with the exec env of the test, false if it fails */
    bool call(const char *name, std::vector<uint32_t> args,
              uint32_t *result = nullptr)
    {
        return call(exec_env, name, args, result);
    }

    bool call(wasm_exec_env_t env, const char *name, std::vector<uint32_t> args,
              uint32_t *result = nullptr)
    {
        wasm_function_inst_t func = wasm_runtime_lookup_function(
            wasm_runtime_get_module_inst(env), name);

        EXPECT_NE(func, nullptr) << name;
        if (!func)
            return false;
        /* room for the result */
        args.resize(args.size() + 2);
        if (!wasm_runtime_call_wasm(env, func, (uint32_t)args.size() - 2,
                                    args.data()))
            return false;
        if (result)
            *result = args[0];
        return true;
    }

    /* Wait until the instance has an exception, which a thread that ends
       with one gives to all the threads of the instance */
    bool wait_for_exception(int milliseconds)
    {
        for (int waited = 0; waited < milliseconds; waited += 10) {
            if (wasm_runtime_get_exception(module_inst))
                return true;
            os_usleep(10000);
        }
        return wasm_runtime_get_exception(module_inst) != nullptr;
    }

  public:
    RuntimeInitArgs init_args;
    char error_buf[256];
    std::vector<uint8_t> module_buf;
    uint32_t stack_size = 64 * 1024;
    wasm_module_t module = nullptr;
    wasm_module_inst_t module_inst = nullptr;
    wasm_exec_env_t exec_env = nullptr;
};

/* A thread that a module creates gets half of the instructions that the
   call which creates it has left */
TEST_P(InstructionLimitThreadTest, thread_of_the_module_has_a_limit)
{
    uint32_t counted = 0;

    wasm_runtime_set_instruction_count_limit(exec_env, 70000);
    /* The call fails if the thread has ended by the time it returns */
    call("create_thread", { 1 });

    ASSERT_TRUE(wait_for_exception(20000))
        << "the thread wasn't stopped by the limit";
    EXPECT_STREQ(wasm_runtime_get_exception(module_inst), limit_exception);
    wasm_runtime_clear_exception(module_inst);

    /* It has counted as far as its half let it: the fast interpreter
       counts fewer than the seven instructions each time around */
    wasm_runtime_set_instruction_count_limit(exec_env, -1);
    ASSERT_TRUE(call("counted", {}, &counted))
        << wasm_runtime_get_exception(module_inst);
    EXPECT_GE(counted, 35000u / 7 - 10);
    EXPECT_LE(counted, 35000u);
}

/* and the call goes on with the other half */
TEST_P(InstructionLimitThreadTest, creating_call_keeps_half)
{
    uint32_t result = 0;
    /* Find what counting to 1000 costs, and what it costs after creating
       a thread that ends at once */
    auto cost = [&](const char *name) {
        int low = 0, high = 1000000;

        while (high - low > 1) {
            int middle = low + (high - low) / 2;

            wasm_runtime_clear_exception(module_inst);
            wasm_runtime_set_instruction_count_limit(exec_env, middle);
            if (call(name, { 1000 }, &result))
                high = middle;
            else
                low = middle;
            /* let the thread end */
            os_usleep(1000);
        }
        return high;
    };
    int count = cost("count");
    int count_after_thread = cost("create_thread_and_count");

    EXPECT_GE(count_after_thread, 2 * count);
    EXPECT_LE(count_after_thread, 2 * count + 40);
}

/* So the threads of a module together run no more than the limit allows:
   a chain of threads, each of which creates the next one and ends,
   comes to an end when a thread has too few instructions left to do
   that */
TEST_P(InstructionLimitThreadTest, chain_of_threads_ends)
{
    uint32_t chained = 0;

    wasm_runtime_set_instruction_count_limit(exec_env, 70000);
    call("create_thread", { 3 });

    ASSERT_TRUE(wait_for_exception(20000))
        << "the threads weren't stopped by the limit";
    EXPECT_STREQ(wasm_runtime_get_exception(module_inst), limit_exception);
    wasm_runtime_clear_exception(module_inst);

    /* Each thread had half of what the one before it had */
    wasm_runtime_set_instruction_count_limit(exec_env, -1);
    ASSERT_TRUE(call("chained", {}, &chained))
        << wasm_runtime_get_exception(module_inst);
    EXPECT_GE(chained, 8u);
    EXPECT_LE(chained, 17u);
}

/* A thread has no limit if the call that creates it has none */
TEST_P(InstructionLimitThreadTest, thread_of_the_module_without_a_limit)
{
    uint32_t counted = 0;

    ASSERT_TRUE(call("create_thread", { 1 }))
        << wasm_runtime_get_exception(module_inst);
    EXPECT_FALSE(wait_for_exception(200));
    ASSERT_TRUE(call("counted", {}, &counted))
        << wasm_runtime_get_exception(module_inst);
    EXPECT_GT(counted, 0u);
}

/* An exec env that the host spawns for another thread has the limit of
   the exec env it was spawned from */
TEST_P(InstructionLimitThreadTest, spawned_exec_env_has_the_limit)
{
    uint32_t result = 0;

    wasm_runtime_set_instruction_count_limit(exec_env, 1000);

    wasm_exec_env_t spawned = wasm_runtime_spawn_exec_env(exec_env);
    ASSERT_NE(spawned, nullptr);
    /* It has a module instance of its own */
    wasm_module_inst_t spawned_inst = wasm_runtime_get_module_inst(spawned);
    ASSERT_TRUE(wasm_runtime_set_running_mode(spawned_inst, GetParam()));

    EXPECT_FALSE(call(spawned, "count", { 100000 }, &result));
    EXPECT_STREQ(wasm_runtime_get_exception(spawned_inst), limit_exception);
    wasm_runtime_clear_exception(spawned_inst);
    EXPECT_TRUE(call(spawned, "count", { 10 }, &result))
        << wasm_runtime_get_exception(spawned_inst);
    EXPECT_EQ(result, 10u);

    wasm_runtime_destroy_spawned_exec_env(spawned);
}

INSTANTIATE_TEST_SUITE_P(RunningModes, InstructionLimitThreadTest,
                         testing::ValuesIn(running_modes), running_mode_name);

} /* namespace */
