# FastGoose

Our fork of the Goose compiler (aardappel/goose, C++), kept to make Goose builds fast for our own
projects (Apostasia/Gooseborn, Netraider). It follows upstream: the `upstream` remote is fetch-only,
our changes sit on top of upstream `master` and are merged forward. So keep them small and
self-contained (one concern per commit, upstream's comment style, no reformatting or drive-by
restructuring of upstream code): each is a candidate pull request to upstream, and every stray edit is
a future merge conflict. Sending anything upstream is Daniel's decision. This overrides the global
"restructure freely" rule for upstream's files; our own files (this one, `tools/`) are ours.

## The bar for a compiler change

A speed change must not change results. Before committing one:

1. `python test/run_tests.py --exe <goose.exe> -j 10` gives the same result as the unmodified
   baseline (about 2 minutes; one test, `bce_wrapped_declaration` at -O2, fails on this machine on
   upstream master too).
2. The generated C of both workloads is byte-identical to the baseline's (`goose file.goose -o out.c`,
   then `cmp`).
3. A cache keeps a way to check itself: `GOOSE_VERIFY_NAMEUNITS=1` makes each cached answer be
   compared with the original computation and aborts on a difference. Run the suite and both
   workloads under it, and extend it for any new cache.

## Split output

`goose build --split` (`src/split.h`, and `BuildSplitUnits` in `src/build.h`) cuts the generated C
into units after codegen, by its text; codegen itself is untouched. Check a change to it with
`python tools/check_split.py <goose.exe> <workdir> -j 8 tools/split_fixtures/*.goose samples/*.goose test/codegen/*.goose ...`
(builds each program both ways and compares what they print; it only builds programs that import
gfx, ui or audio, since running them opens windows). Iterate on a few files; the full set of test
folders takes about ten minutes.

## Build, workloads, profiling

- Build: `cmake --build build/dev --config Release --parallel` (about a minute, MSVC). `build/` is
  gitignored and holds everything below.
- Keep the executable under test out of the tree being rebuilt: copy it to `build/cand/goose.exe`
  before a test run. `build/base/goose.exe` is the unmodified baseline; rebuild it after merging
  upstream.
- Workloads: `build/work/apo/engine` and `build/work/nr/engine` are scratch copies of the Apostasia
  and Netraider sources patched to compile on upstream master (`check src/viewer.goose`, Netraider
  with `--include src/viewer_shim.h`). Never build from or edit the projects' own trees: their
  sessions commit to them constantly.
- Profile: `tools/sampler/sampler.cpp` (build with `cl /O2 /EHsc /std:c++17 ... dbghelp.lib` after
  `vcvars64.bat`) run on a `RelWithDebInfo` build: `sampler -ms 40 -top 60 -o report.txt goose.exe
  check ...`. Time with the Release build at High priority; other sessions load the machine, so
  compare runs made back to back.

Upstream's own docs: `docs/implementation.md` (the checker, by section number as the code cites it),
`docs/testing.md`, `docs/command_line.md`.
