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

## How step 3 is built

The parser rewrites a `par for` into a nested function over a sub-range plus a call to it, and marks
the call as a fork. The checker then checks exactly the sequential meaning, nested functions already
receive the enclosing function's variables by pointer, and codegen only turns the marked call into a
fork (an environment struct holding the call's arguments, a thunk, `gs_par_run`). Run sequentially, the
call is the loop.

```
par for i in a..b BODY     =>  { fn __parN(__lo: i64, __hi: i64) { for i in __lo..__hi BODY }  __parN(a, b); }
par for &x, i in E BODY    =>  { fn __parN(__s, __lo: i64, __hi: i64) {
                                     for &x, __j in __s[__lo..__hi] { let i = __lo + __j; BODY } }
                                 __parN(E[..], 0, (E).len); }
par for v in E BODY        =>  the same with a `const if` on __s's type choosing the count form or the
                               element form (BODY cloned into both), counted by par_detail::count(E)
```

- **Owned arrays (R2):** an enclosing variable `X` that the body reaches only as `X[i]` (i the loop's
  index) or as `X.chunk(i, N)` / `X.chunk_at(B, i)` (N, B an identifier or literal) is passed in as a
  parameter `X` taking `X[..]`, which shadows the outer one, so its writes are the iteration's own and it
  cannot grow. `&x` over E owns E's elements, so E's root must not appear in the body.
- **R1:** inside the nested function every variable of a function around it is read-only
  (`ParCaptured`, applied where the checker decides writability); a refused write says why and what the
  body may write instead (`ParExplain`, a hook in `TypeCheck::Error`). The body may not redeclare the
  loop's names, an owned `X`, N or B (`ParDeclare`).
- **R4:** `return` and a `break` of the marked loop are checker errors; `continue` ends the iteration.
- **R3:** a pass after checking, over the nested function's specialization and everything it calls:
  print, exit, threads and queues, a write to a global, an extern that is not pure (pure: from `std`,
  `math` or `binary`, with only scalar or `const` view parameters).
- **Names:** the nested function is `__par<file>_<n>`, by file and order, so a file parses to the same
  program each time (`goose format --verify` compares two parses).
- **Stackless only:** a body whose specialization needs `gs_sp` runs sequentially until step 5.
- **Sequential fallbacks:** the JIT (`goose run`), `GOOSE_THREADS=1`, a fork inside a fork, a fork in a
  contained worker.

## Checks for each step

- The suite passes (`--cc clang`), and MSVC before a publish.
- Every `par for` test program prints the same bytes at `GOOSE_THREADS=1` and at the core count.
- One `test/errors_tc/par_for_*.goose` fixture per rule; `test/codegen/par_for.goose` runs every form.
- For Netraider: a measured example of the fork/join cost, and a tree walk with growable locals.
