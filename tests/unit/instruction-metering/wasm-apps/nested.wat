;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; Calls nested through native functions. The native functions call an
;; export of this module, which their first argument selects, with their
;; second argument:
;;   0 count   1 count_and_trap   2 ladder
;; call_back passes a failure of that call on, call_back_and_swallow
;; clears it and returns -1.
(module
  (import "env" "call_back"
    (func $call_back (param i32 i32) (result i32)))
  (import "env" "call_back_and_swallow"
    (func $call_back_and_swallow (param i32 i32) (result i32)))
  (import "env" "set_limit" (func $set_limit (param i32)))
  (import "env" "run_down" (func $run_down (param i32)))
  (import "env" "leave" (func $leave (param i32 i32)))
  (import "env" "look" (func $look))
  (import "env" "twice" (func $twice (param i32) (result i32)))
  (import "env" "add5"
    (func $add5 (param i32 i32 i32 i32 i32) (result i32)))
  (type $unary (func (param i32) (result i32)))
  (table 1 funcref)
  (elem (i32.const 0) $twice)
  (memory 1)
  ;; how often a call_back_and_swallow has returned
  (global $returned (mut i32) (i32.const 0))

  ;; count to n
  (func $count (export "count") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $i))

  ;; count to n and then read outside the memory
  (func (export "count_and_trap") (param $n i32) (result i32)
    (drop (call $count (local.get $n)))
    (i32.load (i32.const 0x7ffffff0)))

  ;; d native functions on top of each other, then count to 100
  (func (export "ladder") (param $d i32) (result i32)
    (if (result i32) (local.get $d)
      (then (call $call_back (i32.const 2)
                             (i32.sub (local.get $d) (i32.const 1))))
      (else (call $count (i32.const 100)))))

  ;; d calls on top of each other in this module, then count to n
  (func $deep (export "deep") (param $d i32) (param $n i32) (result i32)
    (if (result i32) (local.get $d)
      (then (call $deep (i32.sub (local.get $d) (i32.const 1))
                        (local.get $n)))
      (else (call $count (local.get $n)))))

  ;; k times: have a native function call count (n)
  (func (export "call_count") (param $k i32) (param $n i32) (result i32)
    (local $sum i32)
    (block $done
      (loop $again
        (br_if $done (i32.eqz (local.get $k)))
        (local.set $k (i32.sub (local.get $k) (i32.const 1)))
        (local.set $sum
          (i32.add (local.get $sum)
                   (call $call_back (i32.const 0) (local.get $n))))
        (br $again)))
    (local.get $sum))

  ;; k times: have a native function call the export `which` with n, and
  ;; swallow its failure
  (func (export "swallow") (param $which i32) (param $k i32) (param $n i32)
    (result i32)
    (local $sum i32)
    (block $done
      (loop $again
        (br_if $done (i32.eqz (local.get $k)))
        (local.set $k (i32.sub (local.get $k) (i32.const 1)))
        (local.set $sum
          (i32.add (local.get $sum)
                   (call $call_back_and_swallow (local.get $which)
                                                (local.get $n))))
        (global.set $returned (i32.add (global.get $returned) (i32.const 1)))
        (br $again)))
    (local.get $sum))
  (func (export "returned") (result i32) (global.get $returned))

  ;; set a new instruction count limit in the middle of a call
  (func (export "set_limit_and_count") (param $limit i32) (param $n i32)
    (result i32)
    (call $set_limit (local.get $limit))
    (call $count (local.get $n)))

  ;; have a native function leave this call only `left` instructions to
  ;; count down, and count to n
  (func (export "run_down_and_count") (param $left i32) (param $n i32)
    (result i32)
    (call $run_down (local.get $left))
    (call $count (local.get $n)))

  ;; have a native function leave this call `left` instructions to count
  ;; down and `reserve` more beyond them, and count to n
  (func (export "leave_and_count") (param $left i32) (param $reserve i32)
    (param $n i32) (result i32)
    (call $leave (local.get $left) (local.get $reserve))
    (call $count (local.get $n)))

  ;; call an imported function through the table, and one with five
  ;; parameters while the result of the first is on the stack: 2 x - 15
  (func (export "import_through_table") (param $x i32) (result i32)
    (call_indirect (type $unary) (local.get $x) (i32.const 0))
    (call $add5 (i32.const 1) (i32.const 2) (i32.const 3) (i32.const 4)
                (i32.const 5))
    (i32.sub))

  ;; tail calls of an imported function, directly and through the table
  (func $tail_call_import (export "tail_call_import") (param $x i32)
    (result i32)
    (return_call $twice (local.get $x)))
  (func (export "tail_call_import_through_table") (param $x i32) (result i32)
    (return_call_indirect (type $unary) (local.get $x) (i32.const 0)))
  ;; and from a function that is called by a function of the module
  (func (export "call_tail_call_import") (param $x i32) (result i32)
    (i32.add (call $tail_call_import (local.get $x)) (i32.const 1)))

  ;; have a native function look at what this call has left, and count
  ;; to n
  (func (export "look_and_count") (param $n i32) (result i32)
    (call $look)
    (call $count (local.get $n)))
)
