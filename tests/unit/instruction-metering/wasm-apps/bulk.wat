;; Copyright (C) 2026 WAMR Community.  All rights reserved.
;; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

;; The instructions that handle a block of memory or of a table. Each
;; function has one of them, for n bytes or n table elements.
(module
  (memory 4)
  (table 2048 funcref)
  (data $data
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef")
  (elem $elem func
    $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f
    $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f
    $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f
    $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f $f)
  ;; a function reference in the table to fill it with
  (elem (i32.const 2040) $f)
  (func $f)

  (func (export "memory_fill") (param $n i32) (result i32)
    (memory.fill (i32.const 0) (i32.const 7) (local.get $n))
    (i32.load8_u (i32.const 0)))

  (func (export "memory_copy") (param $n i32) (result i32)
    (i32.store8 (i32.const 0) (i32.const 7))
    (memory.copy (i32.const 131072) (i32.const 0) (local.get $n))
    (i32.load8_u (i32.const 131072)))

  (func (export "memory_init") (param $n i32) (result i32)
    (memory.init $data (i32.const 0) (i32.const 0) (local.get $n))
    (i32.load8_u (i32.const 0)))

  (func (export "table_fill") (param $n i32) (result i32)
    (table.fill (i32.const 0) (table.get (i32.const 2040)) (local.get $n))
    (local.get $n))

  (func (export "table_copy") (param $n i32) (result i32)
    (table.copy (i32.const 1024) (i32.const 0) (local.get $n))
    (local.get $n))

  (func (export "table_init") (param $n i32) (result i32)
    (table.init $elem (i32.const 0) (i32.const 0) (local.get $n))
    (local.get $n))

  ;; fill all of the memory, again and again
  (func (export "memory_fill_without_end")
    (loop $again
      (memory.fill (i32.const 0) (i32.const 7) (i32.const 262144))
      (br $again)))
)
