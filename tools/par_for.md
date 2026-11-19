# `par for` in FastGoose

Our own implementation of upstream's `docs/design/child_threads.md`, asked for by Netraider (its layout
engine's parallel style and layout pass) and approved by Daniel. Wouter has said he is building his own
version ("in the works", aardappel/goose#234, after closing PR #285); when it lands upstream we switch to
it and port Netraider's code. So this follows the note as closely as possible, keeps the note's syntax,
and is kept separable from upstream's code.

Code that uses `par for` needs our compiler. The README section of ours says so.

## Order (the note's §7, steps 1-3 then 5)

1. `chunk(k, n)` and `chunk_at(bounds, k)` in `std`: done.
2. The effect summary: per specialization, whether it (transitively) writes a global, calls a queue or
   thread builtin, prints or writes stderr, exits, or calls an extern that is not `pure`.
3. `par for`, option A: the keyword; R1 (captured places read-only), R2 structural (`for &x`, `chunk`,
   `chunk_at` with the loop index), R3 (no effects), R4 (no exits across the body), R5; stackless bodies
   only; outlining with an environment struct; the pool; `GOOSE_THREADS`; sequential under TinyCC.
4. (`simd for`, the note's step 4: not asked for yet.)
5. Child programs: bodies with growable locals, a stack block per pool thread, rewound per task.

## Checks for each step

- The suite passes (`--cc clang`), and MSVC before a publish.
- Every `par for` test program prints the same bytes at `GOOSE_THREADS=1` and at the core count.
- One `test/errors_tc` fixture per rule.
- For Netraider: a measured example of the fork/join cost, and a tree walk with growable locals.
