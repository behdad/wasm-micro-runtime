;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; A module that creates threads. The exported globals are what the
;; runtime finds the auxiliary stack by, which it gives a part of to each
;; thread.
(module
  (import "env" "pthread_create"
    (func $pthread_create (param i32 i32 i32 i32) (result i32)))
  (memory 1 1 shared)
  (table 4 funcref)
  (elem (i32.const 1) func $count_without_end $do_nothing $chain)
  (global $stack_pointer (mut i32) (i32.const 32768))
  (global (export "__data_end") i32 (i32.const 1024))
  (global (export "__heap_base") i32 (i32.const 32768))

  ;; A thread that counts in the memory without end, seven instructions
  ;; each time around
  (func $count_without_end (param i32) (result i32)
    (loop $again
      (i32.store (i32.const 64)
                 (i32.add (i32.load (i32.const 64)) (i32.const 1)))
      (br $again))
    (i32.const 0))

  ;; A thread that ends at once
  (func $do_nothing (param i32) (result i32)
    (i32.const 0))

  ;; A thread that counts itself, creates the next thread like it, and
  ;; ends
  (func $chain (param i32) (result i32)
    (drop (i32.atomic.rmw.add (i32.const 68) (i32.const 1)))
    (drop (call $pthread_create (i32.const 16) (i32.const 0) (i32.const 3)
                                (i32.const 0)))
    (i32.const 0))

  ;; create the thread which is the n-th function of the table
  (func $create (export "create_thread") (param $n i32) (result i32)
    (call $pthread_create (i32.const 16) (i32.const 0) (local.get $n)
                          (i32.const 0)))

  ;; how far the thread that counts without end has counted
  (func (export "counted") (result i32) (i32.load (i32.const 64)))

  ;; how many threads of the chain there have been
  (func (export "chained") (result i32) (i32.atomic.load (i32.const 68)))

  ;; count to n
  (func $count (export "count") (param $n i32) (result i32)
    (local $i i32)
    (block $done
      (loop $again
        (br_if $done (i32.ge_u (local.get $i) (local.get $n)))
        (local.set $i (i32.add (local.get $i) (i32.const 1)))
        (br $again)))
    (local.get $i))

  ;; create a thread that ends at once, and count to n
  (func (export "create_thread_and_count") (param $n i32) (result i32)
    (drop (call $create (i32.const 2)))
    (call $count (local.get $n)))
)
