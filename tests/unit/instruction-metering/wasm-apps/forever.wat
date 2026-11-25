;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; Functions that never return: loops and recursions of every shape, which
;; an instruction count limit has to stop. The recursions end with a stack
;; overflow if nothing else stops them.
(module
  (type $void (func))
  (table 2 funcref)
  (elem (i32.const 0) $recursion_indirect $leaf)

  (func $leaf)

  ;; loops that go on through each kind of branch
  (func (export "loop_br") (loop $again (br $again)))
  (func (export "loop_br_if") (loop $again (br_if $again (i32.const 1))))
  (func (export "loop_br_if_local") (param i32)
    (local.set 0 (i32.const 1))
    (loop $again (br_if $again (local.get 0))))
  (func (export "loop_br_table")
    (loop $again (br_table $again $again (i32.const 0))))
  (func (export "loop_br_table_local") (param i32)
    (loop $again
      (block $out (br_table $out $again $again (i32.const 1)))
      (br $again)))
  ;; from inside nested blocks and ifs
  (func (export "loop_if") (loop $again (if (i32.const 1) (then (br $again)))))
  (func (export "loop_if_local") (param i32)
    (local.set 0 (i32.const 1))
    (loop $again (if (local.get 0) (then (br $again)))))
  (func (export "loop_if_else") (param i32)
    (loop $again
      (if (local.get 0) (then (br $again)) (else (br $again)))))
  (func (export "loop_block")
    (loop $again (block (block (br $again)))))
  ;; a loop in a loop: the inner one ends, the outer one doesn't
  (func (export "loop_in_loop") (param i32)
    (loop $outer
      (local.set 0 (i32.const 3))
      (loop $inner
        (local.set 0 (i32.sub (local.get 0) (i32.const 1)))
        (br_if $inner (local.get 0)))
      (br $outer)))
  ;; a loop that carries a value around
  (func (export "loop_param")
    (i32.const 0)
    (loop $again (param i32)
      (i32.add (i32.const 1))
      (br $again)))
  ;; loops whose body is only a call
  (func (export "loop_call") (loop $again (call $leaf) (br $again)))
  (func (export "loop_call_indirect")
    (loop $again (call_indirect (type $void) (i32.const 1)) (br $again)))

  ;; recursions
  (func $recursion (export "recursion") (call $recursion))
  (func $recursion_indirect (export "recursion_indirect")
    (call_indirect (type $void) (i32.const 0)))
  (func $ping (export "recursion_mutual") (call $pong))
  (func $pong (call $ping))
)
