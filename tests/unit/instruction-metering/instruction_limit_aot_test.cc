/*
 * Copyright (C) 2026 WAMR Community.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include <fstream>
#include <iterator>
#include <vector>

#include "gtest/gtest.h"
#include "wasm_runtime_common.h"
#include "bh_platform.h"

/*
 * The case of the suite for the aot mode, where the runtime may be built
 * without the interpreter. AOT code doesn't count instructions, and this
 * is what a host has to find out.
 */

class InstructionLimitAotTest : public testing::Test
{
  protected:
    void SetUp() override
    {
        std::ifstream file("control.aot", std::ios::binary);

        memset(&init_args, 0, sizeof(RuntimeInitArgs));
        init_args.mem_alloc_type = Alloc_With_System_Allocator;
        ASSERT_TRUE(wasm_runtime_full_init(&init_args));

        ASSERT_TRUE(file.good()) << "cannot read control.aot";
        module_buf.assign(std::istreambuf_iterator<char>(file),
                          std::istreambuf_iterator<char>());
        module =
            wasm_runtime_load(module_buf.data(), (uint32_t)module_buf.size(),
                              error_buf, sizeof(error_buf));
        ASSERT_NE(module, nullptr) << error_buf;
        module_inst = wasm_runtime_instantiate(module, stack_size, 0, error_buf,
                                               sizeof(error_buf));
        ASSERT_NE(module_inst, nullptr) << error_buf;
        exec_env = wasm_runtime_create_exec_env(module_inst, stack_size);
        ASSERT_NE(exec_env, nullptr);
    }

    void TearDown() override
    {
        if (exec_env)
            wasm_runtime_destroy_exec_env(exec_env);
        if (module_inst)
            wasm_runtime_deinstantiate(module_inst);
        if (module)
            wasm_runtime_unload(module);
        wasm_runtime_destroy();
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

TEST_F(InstructionLimitAotTest, ignored_by_an_aot_module)
{
    wasm_function_inst_t func =
        wasm_runtime_lookup_function(module_inst, "loop_br");
    uint32_t argv[1] = { 100000 };

    ASSERT_NE(func, nullptr);

    EXPECT_FALSE(wasm_runtime_is_instruction_count_limit_enforced(module_inst));
    /* An AOT module instance takes any running mode, and runs the same
       code in each of them */
    EXPECT_TRUE(wasm_runtime_set_running_mode(module_inst, Mode_Interp));
    EXPECT_FALSE(wasm_runtime_is_instruction_count_limit_enforced(module_inst));

    /* The loop goes around 100000 times under a limit of 10 */
    wasm_runtime_set_instruction_count_limit(exec_env, 10);
    ASSERT_TRUE(wasm_runtime_call_wasm(exec_env, func, 1, argv))
        << wasm_runtime_get_exception(module_inst);
    EXPECT_EQ(argv[0], 100000u);
}
