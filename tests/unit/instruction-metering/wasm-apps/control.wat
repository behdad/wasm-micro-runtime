;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; One function for each control construct, alone and nested. Each takes
;; an i32 and returns an i32. The tests compare the number of instructions
;; that a call of each of them is counted as with what the classic
;; interpreter dispatches for it.
(module
  (type $unary (func (param i32) (result i32)))
  (type $pair (func (param i32) (result i32 i32)))
  (memory 1)
  (global $g (mut i32) (i32.const 0))
  (table 2 funcref)
  (elem (i32.const 0) $add1 $sub1)

  (func $add1 (type $unary) (i32.add (local.get 0) (i32.const 1)))
  (func $sub1 (type $unary) (i32.sub (local.get 0) (i32.const 1)))
  (func $pair (type $pair) (local.get 0) (i32.const 2))

  ;; no control flow
  (func (export "straight") (param i32) (result i32)
    (nop)
    (i32.mul (i32.add (local.get 0) (i32.const 1)) (i32.const 3)))

  ;; block
  (func (export "block") (param i32) (result i32)
    (block (result i32) (local.get 0)))
  (func (export "block_in_block") (param i32) (result i32)
    (block (result i32)
      (block (result i32)
        (block (result i32) (local.get 0)))))

  ;; a loop that nothing branches back to
  (func (export "loop_once") (param i32) (result i32)
    (loop (result i32) (local.get 0)))

  ;; if without else: when the condition is false the interpreter jumps
  ;; over the end opcode
  (func (export "if") (param i32) (result i32)
    (local $r i32)
    (if (local.get 0) (then (local.set $r (i32.const 5))))
    (local.get $r))
  ;; if with else: the if branch runs the else opcode, which jumps to the
  ;; end opcode; the else branch doesn't
  (func (export "if_else") (param i32) (result i32)
    (if (result i32) (local.get 0) (then (i32.const 1)) (else (i32.const 2))))
  ;; conditions that are known when the code is compiled
  (func (export "if_true") (param i32) (result i32)
    (local $r i32)
    (if (i32.const 1) (then (local.set $r (local.get 0))))
    (local.get $r))
  (func (export "if_false") (param i32) (result i32)
    (local $r i32)
    (if (i32.const 0) (then (local.set $r (local.get 0))))
    (local.get $r))
  (func (export "if_else_true") (param i32) (result i32)
    (if (result i32) (i32.const 1) (then (local.get 0)) (else (i32.const 9))))
  (func (export "if_else_false") (param i32) (result i32)
    (if (result i32) (i32.const 0) (then (i32.const 9)) (else (local.get 0))))
  ;; an if in each branch of an if
  (func (export "if_in_if") (param i32) (result i32)
    (if (result i32) (i32.and (local.get 0) (i32.const 1))
      (then
        (if (result i32) (i32.and (local.get 0) (i32.const 2))
          (then (i32.const 3))
          (else (i32.const 1))))
      (else
        (if (result i32) (i32.and (local.get 0) (i32.const 2))
          (then (i32.const 2))
          (else (i32.const 0))))))
  ;; an if without else in an if without else
  (func (export "if_then_if") (param i32) (result i32)
    (local $r i32)
    (if (i32.and (local.get 0) (i32.const 1))
      (then
        (local.set $r (i32.const 1))
        (if (i32.and (local.get 0) (i32.const 2))
          (then (local.set $r (i32.const 3))))))
    (local.get $r))

  ;; br out of one block and out of several
  (func (export "br") (param i32) (result i32)
    (block (result i32) (br 0 (local.get 0))))
  (func (export "br_outer") (param i32) (result i32)
    (block $a (result i32)
      (block $b (result i32)
        (block $c (result i32) (br $a (local.get 0))))))
  ;; br_if
  (func (export "br_if") (param i32) (result i32)
    (block (result i32)
      (drop (br_if 0 (i32.const 5) (local.get 0)))
      (i32.const 6)))
  ;; br_if whose value is carried over another value on the stack
  (func (export "br_if_value") (param i32) (result i32)
    (i32.add
      (i32.const 100)
      (block (result i32)
        (drop (br_if 0 (i32.const 5) (local.get 0)))
        (i32.const 6))))
  ;; two br_if in a row, and one out of an if
  (func (export "br_if_twice") (param i32) (result i32)
    (local $r i32)
    (block $out
      (br_if $out (i32.eq (local.get 0) (i32.const 1)))
      (br_if $out (i32.eq (local.get 0) (i32.const 2)))
      (if (local.get 0)
        (then
          (local.set $r (i32.const 7))
          (br_if $out (i32.eq (local.get 0) (i32.const 3)))
          (local.set $r (i32.const 8))))
      (local.set $r (i32.add (local.get $r) (i32.const 10))))
    (local.get $r))
  ;; br_table to blocks
  (func (export "br_table") (param i32) (result i32)
    (block $d (result i32)
      (block $c
        (block $b
          (block $a (br_table $a $b $c (local.get 0)))
          (br $d (i32.const 10)))
        (br $d (i32.const 20)))
      (i32.const 30)))

  ;; a loop that goes on through a br_if at its end
  (func (export "loop_br_if") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $i))
  ;; a loop that is left through a br_if and goes on through a br
  (func (export "loop_br") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $i))
  ;; a loop that goes on through a br_table
  (func (export "loop_br_table") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $again
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br_table $done $again
          (i32.lt_u (local.get $i) (local.get $n)))))
    (local.get $i))
  ;; a loop that goes on from inside an if
  (func (export "loop_if") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (if (i32.lt_u (local.get $i) (local.get $n)) (then (br $again))))
    (local.get $i))
  ;; a loop in a loop, with an if in it
  (func (export "loop_in_loop") (param $n i32) (result i32)
    (local $i i32) (local $j i32) (local $s i32)
    (loop $outer
      (local.set $j (i32.const 0))
      (loop $inner
        (if (i32.and (local.get $j) (i32.const 1))
          (then (local.set $s (i32.add (local.get $s) (local.get $j))))
          (else (local.set $s (i32.sub (local.get $s) (i32.const 1)))))
        (local.set $j (i32.add (local.get $j) (i32.const 1)))
        (br_if $inner (i32.lt_u (local.get $j) (local.get $i))))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $outer (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $s))
  ;; a loop with a parameter, which a branch back to it carries
  (func (export "loop_param") (param $n i32) (result i32)
    (local $i i32)
    (i32.const 0)
    (loop $again (param i32) (result i32)
      (i32.add (i32.const 3))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n)))))

  ;; return, and a branch to the function itself
  (func (export "return") (param i32) (result i32)
    (if (local.get 0) (then (return (i32.const 1))))
    (i32.const 2))
  (func (export "br_func") (param i32) (result i32)
    (i32.const 3)
    (br_if 0 (local.get 0))
    (drop)
    (i32.const 4))
  ;; code that is never reached
  (func (export "dead") (param i32) (result i32)
    (block (result i32)
      (br 0 (local.get 0))
      (drop (i32.const 2))
      (i32.const 3)))
  (func (export "dead_in_if") (param i32) (result i32)
    (if (result i32) (local.get 0)
      (then
        (return (i32.const 1))
        (i32.const 2))
      (else (i32.const 3))))

  ;; calls
  (func (export "call") (param i32) (result i32) (call $add1 (local.get 0)))
  (func (export "call_in_call") (param i32) (result i32)
    (call $add1 (call $sub1 (call $add1 (local.get 0)))))
  (func (export "call_indirect") (param i32) (result i32)
    (call_indirect (type $unary)
      (local.get 0) (i32.and (local.get 0) (i32.const 1))))
  (func $factorial (export "recursion") (param i32) (result i32)
    (if (result i32) (i32.le_u (local.get 0) (i32.const 1))
      (then (i32.const 1))
      (else
        (i32.mul
          (local.get 0)
          (call $factorial (i32.sub (local.get 0) (i32.const 1)))))))
  ;; a loop whose body is a call
  (func (export "loop_call") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (local.set $i (call $add1 (local.get $i)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $i))

  ;; blocks with a parameter and with several results
  (func (export "multi_value") (param i32) (result i32)
    (local.get 0)
    (block (param i32) (result i32 i32) (i32.const 7))
    (i32.add)
    (if (param i32) (result i32) (local.get 0)
      (then (i32.add (i32.const 1)))
      (else (i32.sub (i32.const 1))))
    (call $pair)
    (i32.add))

  ;; memory, a global, the table and select
  (func (export "memory") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (i32.store (i32.shl (local.get $i) (i32.const 2)) (local.get $i))
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n))))
    (global.set $g (i32.load (i32.const 4)))
    (memory.fill (i32.const 1024) (i32.const 1) (i32.const 16))
    (select
      (call_indirect (type $unary) (global.get $g) (i32.const 0))
      (i32.load8_u (i32.const 1030))
      (memory.size)))

  ;; a trap in the middle of straight-line code
  (func (export "trap") (param i32) (result i32)
    (i32.add (i32.div_u (i32.const 100) (local.get 0)) (i32.const 1)))

  ;; A call sets the local variables of the function to zero and counts
  ;; one more instruction for every 64 bytes of them: none for the 56
  ;; bytes of the first function, five for the 320 bytes of the second
  (func $few_locals (export "few_locals") (param i32) (result i32)
    (local i64 i64 i64 i64 i64 i64 i64)
    (local.get 0))
  (func $many_locals (export "many_locals") (param i32) (result i32)
    (local i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)
    (local i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)
    (local i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)
    (local i64 i64 i64 i64 i64 i64 i64 i64 i64 i64)
    (local.get 0))
  (func (export "loop_call_few_locals") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (drop (call $few_locals (local.get $i)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $i))
  (func (export "loop_call_many_locals") (param $n i32) (result i32)
    (local $i i32)
    (loop $again
      (local.set $i (i32.add (local.get $i) (i32.const 1)))
      (drop (call $many_locals (local.get $i)))
      (br_if $again (i32.lt_u (local.get $i) (local.get $n))))
    (local.get $i))
)
