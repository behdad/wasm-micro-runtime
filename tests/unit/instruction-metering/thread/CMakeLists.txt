# Copyright (C) 2026 WAMR Community.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The instruction count limit and the threads of a module, in the modes
# that enforce the limit.
wamr_unit_test_suite_run_modes(instruction-metering-thread
  MODES classic-interp fast-interp fast-jit)
if(NOT WAMR_UNIT_TEST_SUITE_ENABLED)
  return()
endif()

set (WAMR_BUILD_INSTRUCTION_METERING 1)
set (WAMR_BUILD_LIB_PTHREAD 1)

include (../../unit_common.cmake)

add_executable (instruction_metering_thread_test
  ${CMAKE_CURRENT_SOURCE_DIR}/instruction_limit_thread_test.cc
  ${WAMR_RUNTIME_LIB_SOURCE}
)
target_link_libraries (instruction_metering_thread_test gtest_main)

wamr_unit_test_compile_wat_to_wasm (
  TARGET instruction_metering_thread_test
  SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wasm-apps/thread.wat
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/thread.wasm
)

gtest_discover_tests (instruction_metering_thread_test)
