;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; What the runtime runs by itself: the functions that it runs when the
;; module is instantiated, and the malloc and free that the module
;; exports. Each of them goes around a loop as often as a native function
;; tells it to, and then sets its bit in a global:
;;   0 the start function   1 __post_instantiate   2 __wasm_call_ctors
;;   3 malloc               4 free
(module
  (import "env" "rounds" (func $rounds (param i32) (result i32)))
  (import "env" "ask_for_memory"
    (func $ask_for_memory (param i32) (result i32)))
  (memory 1)
  (global $ran (mut i32) (i32.const 0))

  (func $run (param $which i32)
    (local $n i32)
    (local.set $n (call $rounds (local.get $which)))
    (block $done
      (loop $again
        (br_if $done (i32.eqz (local.get $n)))
        (local.set $n (i32.sub (local.get $n) (i32.const 1)))
        (br $again)))
    (global.set $ran
      (i32.or (global.get $ran) (i32.shl (i32.const 1) (local.get $which)))))

  (func $start (call $run (i32.const 0)))
  (start $start)
  (func (export "__post_instantiate") (call $run (i32.const 1)))
  (func (export "__wasm_call_ctors") (call $run (i32.const 2)))
  (func (export "malloc") (param i32) (result i32)
    (call $run (i32.const 3))
    (i32.const 1024))
  (func (export "free") (param i32) (call $run (i32.const 4)))

  ;; have a native function ask the runtime for memory of the module and
  ;; give it back, which runs the malloc and the free above
  (func (export "malloc_by_native") (param $size i32) (result i32)
    (call $ask_for_memory (local.get $size)))

  ;; which of them have run to their end
  (func (export "ran") (result i32) (global.get $ran))

  ;; count to n
  (func (export "count") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $i))
)
