# Racy views: a worker reading main's arrays

FastGoose's own (not upstream's). Asked for by a JIT that compiles on a worker while main keeps
running and changing the heap the compiler reads. Copying a snapshot per compile would copy most
of the heap, so the worker reads main's arrays in place. It accepts that what it reads may be
stale or inconsistent, because it checks its result against an epoch before main installs it.

## The rule that makes it safe

Memory safety must never depend on what the reader sees. Goose's memory model gives most of
this already:

- Nothing moves (§1.3). A resizable array's elements stay at its header's `base` for the array's
  whole life, and growth only bumps the top. So `base`, taken when the view is made, stays
  right, and only the length races.
- A data stack is one reservation that is never released while the program runs. Popping or
  shrinking only lowers the length and the top.

So a view records `base`, the address of the header's `len`, and `max`, the element count from
`base` to the end of the region `base` lies in. Every read checks `i < min(len now, max)`, so no
read leaves mapped memory, whatever length it sees: stale, from a later array that reused the
stack, or garbage.

Windows commits a region's pages on first touch, through a fault handler that knows only its own
thread's regions. The handler on a worker therefore also knows main's registry
(`gs_main_regions`) and commits a page there that a view touches. Such a page reads as zeros.
POSIX maps the whole usable reservation, so a read never faults there.

## What the compiler emits

- `racy T[:]` is the C struct `gs_racy` (`base`, `len`, `max`). `v.len` is `gs_racy_len(v)`.
  `v[i]` is bounds-checked against it, then the element is copied out with `gs_racy_copy`, one
  volatile load per word.
  - A single load matters. In `j = v[k]; if (j < n) use(t[j])`, a compiler may legally load
    `v[k]` again for the use, since it assumes no race, and get an unchecked value. A volatile
    load is performed exactly once.
  - After the copy, the element is validated: a bool reads as `!= 0`, and an enum's tag (and
    those nested in its payload) must name a variant, or the read fails a check. A garbage tag
    fed to a `match` could otherwise jump anywhere.
- `racy S`, for a struct `S`, is a pointer to main's `S` plus a `gs_racy` for each resizable
  field. Each view's `base` and `max` are taken when the view is made. A scalar field reads as
  one load; a resizable field reads as its `racy T[:]`. Reading a reference field is a compile
  error.
- Main's own code is unchanged: no barrier, no check, and no change to the C of a program
  without views. Formally, main's plain stores race with the worker's loads (C11 data races), but
  nothing a compiler may do to those stores (reorder, delay, sink them out of a loop) can break
  the bounds above. It can only make a value staler.

## The language side

- `racy(x)` makes a view, on main only (a worker's call panics: its arrays end with it): of a
  resizable array `T[>..]` or `T[>..<]`, giving `racy T[:]`, or of a fixed-size struct, giving
  `racy S`. Elements must be plain data: integers, floats, bools, and fixed arrays, structs and
  fixed-mode enums of those. No references, slices, varints or index types at any depth (an index
  type would index without a check).
- A view is read-only: `v[i]`, `v.len`, and the fields of a struct view. A field that is a
  reference to a resizable array reads as that array's `racy T[:]`; any other reference field is a
  compile error when read, so a struct may hold some (an embedder's context) as long as the
  worker leaves them alone.
- Every read is a fresh copy, a temporary: writing through a view, `&v[i]`, or a mutable reference
  to an element is a compile error. `for x in v` is one too: read by index, so that each element
  is loaded once, where the code says.
- A view is its own type, so it never reaches a `T[:]` or `const T[:]` parameter, code compiled
  for plain loads. An untyped parameter specializes on it like any other type, so one body
  serves both a `Vm&` and a `racy Vm`, and a write in that body fails to compile only in the view's
  specialization.
- A view lives in a local, a parameter or a result, never in a field, an array or a global. It
  crosses into a worker as a `thread_spawn` argument.
- Lifetime: main's regions outlive every worker (workers are killed when main returns), so a view
  never reads unmapped memory. A view of an array whose scope has ended reads whatever reused the
  stack, which is safe and only stale. The view is meant for a worker that checks what it computed
  against an epoch before main uses it.
