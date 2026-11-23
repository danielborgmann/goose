# `par for` in FastGoose

Our own implementation of upstream's `docs/design/child_threads.md`, asked for by Netraider (its layout
engine's parallel style and layout pass) and approved by Daniel. Wouter has said he is building his own
version ("in the works", aardappel/goose#234, after closing PR #285); when it lands upstream we switch to
it and port Netraider's code. So this follows the note as closely as possible, keeps the note's syntax,
and is kept separable from upstream's code.

Code that uses `par for` needs our compiler. The README section of ours says so.

## Order (the note's §7, steps 1-3 then 5)

1. `chunk(k, n)` and `chunk_at(bounds, k)` in `std`: done.
2. The effect summary (done, as R3's pass): per specialization, whether it (transitively) writes a global, calls a queue or
   thread builtin, prints or writes stderr, exits, or calls an extern that is not `pure`.
3. `par for`, option A (done): the keyword; R1 (captured places read-only), R2 structural (`for &x`, `chunk`,
   `chunk_at` with the loop index), R3 (no effects), R4 (no exits across the body), R5; stackless bodies
   first; outlining with an environment struct; the pool; `GOOSE_THREADS`; sequential under TinyCC.
4. (`simd for`, the note's step 4: not asked for yet.)
5. Child programs (done): bodies with growable locals, a stack block per pool thread, rewound per task.

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
  cannot grow. `&x` over E owns E's elements, so E's root must not appear in the body. An array the
  body names only as `X.atomic_or(..)` / `X.atomic_add(..)` / `X.atomic_load(..)` (`ParAtomicForm`) is
  passed in the same way: the iterations share it, each access one relaxed atomic one to an i64 or u64
  word (the fork's join
  orders what main reads after), and no plain access to it can race with them.
- **R1:** inside the nested function every variable of a function around it is read-only
  (`ParCaptured`, applied where the checker decides writability); a refused write says why and what the
  body may write instead (`ParExplain`, a hook in `TypeCheck::Error`). The body may not redeclare the
  loop's names, an owned `X`, N or B (`ParDeclare`).
- **The pool's start.** Each pool thread makes its block of stacks (the most any body needs) as it
  starts, before it claims a chunk, so the first fork waits only for the threads that are ready; the
  program's init names that block's maker (`gs_par_ready`). `par_warm()` starts the pool without a
  fork.
- **R4:** `return` and a `break` of the marked loop are checker errors; `continue` ends the iteration.
- **R3:** a pass after checking, over the nested function's specialization and everything it calls:
  print, exit, threads and queues, a write to a global, an extern that is not pure (pure: from `std`,
  `math` or `binary`, its parameters scalars, `const` views, or writable references or slices of
  numbers, its out-parameters; and `os::clock_ns`, a thread-safe counter read).
- **Globals and references, in the checker.** A syntactic pass cannot follow a reference, so R3's
  global writes are also the checker's. A specialization checked for a call in a par for's iterations
  has `FnSpec::parreach`, part of the key (`InParContext`), and there `ParCaptured` holds for every
  global, so that a global is read-only as a `const` is and every reference taken to it is too
  (§9.5). Since `const` is shallow, a reference read out of a field or an element is in addition writable
  there only if its path is and none of the roots it may point into is read-only (`ParWritableLoad`):
  static data, a global, a captured variable, or the class root of a
  parameter whose call site passed read-only storage (`VarDef::parreadonly`).
- **`GOOSE_PAR_ALL`:** a survey that reports every refusal, not the first per loop: those rules are off
  while checking, `ParSurveyWrites` lists direct writes to captured variables and `ParSurveyEffects` every
  effect with its call chain. It follows no references, so it builds nothing.
- **Names:** the nested function is `__par<file>_<n>`, by file and order, so a file parses to the same
  program each time (`goose format --verify` compares two parses).
- **The fork** (`src/par_for.h`): the call's C arguments, but the range, go into an environment struct
  (its fields are the callee's C parameters, `SigParams`), with the forking thread program's `gs_stks`,
  `gs_nstks` and `gs_gl`; a thunk per call takes those and calls the function on a chunk;
  `gs_par(thunk, &env, lo, hi)` runs it. The optimizer never inlines a par for's function.
- **The pool** (`runtime_threads.h`, `gs_par_run`): made at the first fork, `GOOSE_THREADS` threads in
  all or one per CPU, or under Windows on a hybrid or SMT CPU one per physical core of the highest
  `EfficiencyClass`, each pool thread held to a core of its own (`gs_par_percore`). `par_threads(n)` asks for another
  size; the next fork stops the pool and starts one of that size. A fork is cut into 4 chunks per
  thread, claimed through one word holding the fork's number, chunk count and next chunk; the forking
  thread runs chunks too, then waits for the rest. The pool's threads read that word with plain loads,
so waiting ones share its cache line. They wait for the next fork in UMWAIT where the CPU has it
(`gs_par_umwait`, which a write to the word ends), else in `pause`, for 200 us or
  `par_spin_us`/`GOOSE_PAR_SPIN_US` (a 13900HX's 4096 pauses, the old spin: 170 us on a P core, 75 on
  an E core), then sleep on a condition variable. A program with a par
  for and no threads gets `GS_PAR`, which turns on `GS_NEED_THREADS` except in a JIT run.
- **Bodies that need data stacks** (`gs_sp`): the thunk is written once the stack counts are known
  (`EmitParThunks`). On the forking thread it calls the function at the fork's `gs_sp`; on a pool thread
  (`gs_par_pool_thread`) on the thread's own block (`gs_par_block`), as many stacks as the program's
  neediest par for body, made as the thread starts and kept, every top put back after each
  chunk. The block counts among a worker's regions, which caps the pool by the data stack budget.
- **Sequential fallbacks:** the JIT (`goose run`), `GOOSE_THREADS=1`, a fork inside a fork, a fork in a
  contained worker, a fork while another thread program forks, a range of one iteration.

## Checks for each step

- The suite passes (`--cc clang`), and MSVC before a publish.
- Every `par for` test program prints the same bytes at `GOOSE_THREADS=1` and at the core count.
- One `test/errors_tc/par_for_*.goose` fixture per rule; `test/codegen/par_for.goose` runs every form.
- For Netraider: a measured example of the fork/join cost, and a tree walk with growable locals.
