# Copyright (C) 2026 WAMR Community.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The instruction count limit in a runtime that is built without hardware
# bound checks, in the modes that enforce the limit. The runtime has no
# thread local exec env there, and the code of the Fast JIT returns from
# an exception instead of leaving by a signal.
wamr_unit_test_suite_run_modes(instruction-metering-sw-bound-check
  MODES classic-interp fast-interp fast-jit)
if(NOT WAMR_UNIT_TEST_SUITE_ENABLED)
  return()
endif()

set (WAMR_BUILD_INSTRUCTION_METERING 1)
set (WAMR_DISABLE_HW_BOUND_CHECK 1)

include (../../unit_common.cmake)

add_executable (instruction_metering_test_sw_bound_check
  ${CMAKE_CURRENT_SOURCE_DIR}/../instruction_limit_test.cc
  ${WAMR_RUNTIME_LIB_SOURCE}
)
target_link_libraries (instruction_metering_test_sw_bound_check gtest_main)

foreach (fixture control forever nested init)
  wamr_unit_test_compile_wat_to_wasm (
    TARGET instruction_metering_test_sw_bound_check
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/../wasm-apps/${fixture}.wat
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.wasm
  )
endforeach ()

gtest_discover_tests (instruction_metering_test_sw_bound_check
  TEST_PREFIX "sw_bound_check.")
