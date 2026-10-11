"""Whole-program SPU tests: assembled programs run through the real recovery and emitter.

`gen` assembles four SPU programs with `tests.spu_asm`, recovers and emits each as its own synthetic
image through the `spu-emit` entry points (`spu_recover.recover_image`, `spu_emit.emit_one`,
`spu_emit.output_files`), so the C tables are the ones a real generated tree has. The C++ driver
(`runtime/tests/spu_programs_main.cpp`) places the image bytes in guest memory and runs every program
through the lv2 SPU thread group calls. Expected results come from the arithmetic of each program,
never from running the emitter.
"""

from __future__ import annotations

# Local store layout shared by the programs.
CODE_LS = 0x01000     # entry of every program
TABLE_LS = 0x02000    # data segment of the jump-table program (16-byte aligned)
BUF_LS = 0x04000      # scratch area the programs use (128-byte aligned, outside every segment)

# Guest address of the first image; the images follow at IMAGE_STRIDE. The driver commits these ranges.
IMAGE_EA = 0x00F00000
IMAGE_STRIDE = 0x10000

# The sum program reads the array in chunks of this many bytes.
CHUNK_BYTES = 1024
# Increments per thread of the counter program.
COUNTER_ROUNDS = 10000
# Jump table entries of the table program; larger selectors take the default case.
TABLE_ENTRIES = 4
