# Copyright (C) 2026 WAMR Community.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# The limit is enforced by the interpreters and by the Fast JIT. The suite
# runs in the modes with the LLVM JIT as well, where the interpreter is built
# in too, and in the aot mode: there it checks that the runtime tells that
# the limit is ignored.
wamr_unit_test_suite_run_modes(instruction-metering
  MODES classic-interp fast-interp fast-jit llvm-jit aot multi-tier-jit)
if(NOT WAMR_UNIT_TEST_SUITE_ENABLED)
  return()
endif()

set (WAMR_BUILD_INSTRUCTION_METERING 1)
# bulk.wat has the table instructions
set (WAMR_BUILD_REF_TYPES 1)
# control.wat and nested.wat have tail calls
set (WAMR_BUILD_TAIL_CALL 1)

include (../unit_common.cmake)

# The aot mode (the runtime_lib.cmake included above turns WAMR_BUILD_AOT on
# for the LLVM JIT as well). The runtime may be built without the interpreter
# there, so the cases that run bytecode are left out of it.
if (WAMR_BUILD_AOT EQUAL 1 AND NOT WAMR_BUILD_JIT EQUAL 1)
  add_executable (instruction_metering_test_aot
    ${CMAKE_CURRENT_SOURCE_DIR}/instruction_limit_aot_test.cc
    ${WAMR_RUNTIME_LIB_SOURCE}
  )
  target_link_libraries (instruction_metering_test_aot gtest_main)
  wamr_unit_test_compile_wat_to_wasm (
    TARGET instruction_metering_test_aot
    SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wasm-apps/control.wat
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/control.wasm
  )
  wamr_unit_test_compile_wasm_to_aot (
    TARGET instruction_metering_test_aot
    INPUT ${CMAKE_CURRENT_BINARY_DIR}/control.wasm
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/control.aot
    FLAGS --enable-tail-call
  )
  gtest_discover_tests (instruction_metering_test_aot)
  return ()
endif ()

set (unit_test_sources
  ${CMAKE_CURRENT_SOURCE_DIR}/instruction_metering_test.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/instruction_limit_test.cc
  ${WAMR_RUNTIME_LIB_SOURCE}
)

# Both interpreters check the instruction limit separately in computed goto
# dispatch (1) and switch dispatch (0), so build the suite for both.
foreach (labels_as_values 1 0)
  set (test_target instruction_metering_test_lav${labels_as_values})
  add_executable (${test_target} ${unit_test_sources})
  target_compile_definitions (${test_target} PRIVATE
    WASM_ENABLE_LABELS_AS_VALUES=${labels_as_values})
  target_link_libraries (${test_target} ${LLVM_AVAILABLE_LIBS} gtest_main)

  foreach (fixture control forever nested init bulk)
    wamr_unit_test_compile_wat_to_wasm (
      TARGET ${test_target}
      SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/wasm-apps/${fixture}.wat
      OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/${fixture}.wasm
    )
  endforeach ()

  gtest_discover_tests (${test_target}
    TEST_PREFIX "labels_as_values_${labels_as_values}.")
endforeach ()
