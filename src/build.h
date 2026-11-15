// Goose compiler — `goose build` (docs/design/packages.md §6): the C the
// compiler wrote, the runtime object and the native modules' response files,
// compiled and linked into an executable by the toolchain of toolchain.h.
// Intermediates live under GOOSE_HOME; the executable lands beside the root
// source unless -o says otherwise.
#pragma once

namespace goose {

// In main.cpp: the runtime's text hash, the runtime object's C, and a path's directory.
string RuntimeVersion();
string RuntimeObjectSource();
string DirOf(const string &path);

struct BuildOptions {
    bool debug = false;       // --debug: GS_DEBUG=1, debug information, no C optimization
    bool verbose = false;     // -v: print every command and what it prints
    bool dryrun = false;      // --dry-run: print the commands and run nothing
    bool standalone = false;  // --standalone: no runtime object
    bool shared = false;      // --shared: build a self-contained shared library
    string exe;               // -o: the executable/library; "" puts it beside the root source
    bool split = false;       // --split: the program's C as units (split.h), compiled apart and kept
    int64_t splitkb = 128;    // About how much C a unit defines (GOOSE_SPLIT_KB)
    bool stats = false;       // --stats: also how long the C compile and the link took
};

// Seconds since `t`, which moves on to now.
inline double SecondsSince(std::chrono::steady_clock::time_point &t) {
    auto now = std::chrono::steady_clock::now();
    auto s = std::chrono::duration<double>(now - t).count();
    t = now;
    return s;
}

inline string AbsolutePath(const string &path) {
    std::error_code ec;
    auto abs = filesystem::absolute(filesystem::path(path), ec);
    return ec ? path : abs.lexically_normal().string();
}

// GOOSE_HOME/build/prog/<stem>-<hash of the root's absolute path>/: where a
// program's C, objects and logs go.
inline string ProgramBuildDir(const string &rootfile) {
    auto abs = AbsolutePath(rootfile);
    #ifdef _WIN32
        for (auto &c : abs) c = (char)tolower((unsigned char)c);   // One directory per file, however spelled.
    #endif
    return cat(GooseHome(), "/build/prog/", PathStem(rootfile), "-", Hex8(abs));
}

// Runs the toolchain's commands, printing them as -v and --dry-run ask, and
// a failed one with what it printed.
struct Builder {
    const Toolchain &tc;
    const BuildOptions &opts;
    FILE *msgs;

    // The compile flags of both styles: optimization, or debug information
    // and none of it. Defines go into the C itself (main.cpp), never here.
    vector<string> CFlags() const {
        if (tc.style == "msvc") {
            vector<string> f = { "/nologo" };
            if (opts.debug) f.push_back("/Od"), f.push_back("/Zi");
            else f.push_back("/O2");
            return f;
        }
        if (opts.debug) return { "-O0", "-g" };
        return { "-O2" };
    }

    // The system libraries a program links beyond the C library, in the cc
    // style: none on Windows, where they live in the C runtime.
    vector<string> SystemLibs() const {
        #ifdef _WIN32
            return {};
        #else
            if (tc.style == "msvc") return {};
            vector<string> libs = { "-lm", "-pthread" };
            #ifdef __linux__
                // dlopen, for the runtime's dynamic externs: in libdl before
                // glibc 2.34, and in libc since, where the archive left
                // behind is empty.
                libs.push_back("-ldl");
            #endif
            return libs;
        #endif
    }

    // Runs `argv`, `what` being what it does ("compiling the runtime"), with
    // its output kept in `logfile`. False when it failed, after saying so.
    bool Run(const vector<string> &argv, const string &what, const string &logfile) const {
        if (opts.dryrun || opts.verbose) fprintf(msgs, "%s\n", CommandLineText(argv).c_str());
        if (opts.dryrun) return true;
        fflush(msgs);
        ProcessResult r;
        {
            ProgressStep step(what);
            r = RunProcess(argv, "", opts.verbose);
        }
        if (!logfile.empty()) WriteFileMakingDirs(logfile, r.output);
        if (r.started && r.code == 0) return true;
        auto msg = cat(what, " failed", r.started ? cat(" (exit ", r.code, ")") : string(), ":\n    ",
                       CommandLineText(argv), "\n");
        if (!opts.verbose) msg += r.output;
        if (!logfile.empty())
            Append(msg, msg.back() == '\n' ? "" : "\n", "(the output is in ", logfile, ")");
        ProgressPrint(msg);
        return false;
    }

    // A system library by name, as the style's linker takes it.
    string LibFlag(const string &lib) const {
        return tc.style == "msvc" ? cat(lib, ".lib") : cat("-l", lib);
    }

    // Compiles the C file `src` to the object `obj`, with `extra` flags
    // (a package's includes and defines) after the usual ones.
    bool CompileObject(const string &src, const string &obj, const string &what,
                       const string &logfile, const vector<string> &extra = {}) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        for (auto &f : extra) argv.push_back(f);
        if (tc.style == "msvc") {
            argv.push_back("/c");
            argv.push_back(src);
            argv.push_back(cat("/Fo:", obj));
            if (opts.debug) argv.push_back(cat("/Fd:", obj, ".pdb"));
        } else {
            argv.push_back("-c");
            argv.push_back(src);
            argv.push_back("-o");
            argv.push_back(obj);
        }
        return Run(argv, what, logfile);
    }

    // Links `objects` and the system libraries `libs` (LibFlag spellings)
    // into the shared library `out`, with `builddir` taking the
    // intermediates: a package's JIT library.
    bool LinkShared(const vector<string> &objects, const vector<string> &libs, const string &out,
                    const string &builddir, const string &logfile, const string &what) const {
        vector<string> argv = { tc.cc };
        if (tc.style == "msvc") {
            argv.push_back("/nologo");
            argv.push_back("/LD");
            argv.push_back(cat("/Fe:", out));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            for (auto &o : objects) argv.push_back(o);
            for (auto &l : libs) argv.push_back(l);
        } else {
            argv.push_back("-shared");
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(out);
            for (auto &l : libs) argv.push_back(l);
            for (auto &l : SystemLibs()) argv.push_back(l);
        }
        return Run(argv, what, logfile);
    }

    // Compiles `src` and links it with `objects` and the response files
    // `rsps` into `exe`, with `builddir` taking the intermediates: an
    // executable, or with --shared a shared library, whose C holds the
    // runtime (a shared build is standalone) and whose package objects are
    // position independent already (native.h).
    bool Link(const string &src, const vector<string> &objects, const vector<string> &rsps,
              const string &exe, const string &builddir, const string &logfile) const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        if (tc.style == "msvc") {
            if (opts.shared) argv.push_back("/LD");
            argv.push_back(cat("/Fe:", exe));
            argv.push_back(cat("/Fo:", builddir, "\\"));
            if (opts.debug) argv.push_back(cat("/Fd:", builddir, "\\", PathStem(exe), ".pdb"));
            argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            for (auto &r : rsps) argv.push_back(cat("@", r));
        } else {
            if (opts.shared) {
                // Not on Windows, where clang rejects the flag for its
                // target (the code is position independent regardless).
                #ifndef _WIN32
                    argv.push_back("-fPIC");
                #endif
                argv.push_back("-shared");
            }
            argv.push_back(src);
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(exe);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            for (auto &l : SystemLibs()) argv.push_back(l);
        }
        return Run(argv, cat("building ", exe), logfile);
    }

    // Links the objects the response file `unitsrsp` names (a split
    // program's units) with `objects` and the response files `rsps` into
    // `exe`.
    // GOOSE_SPLIT_LTO=1 (experimental): a split program's units are compiled
    // to ThinLTO bitcode and linked by lld, which inlines across them, with a
    // cache of its own under the units' directory. clang and clang-cl only.
    bool SplitLTO() const {
        auto e = getenv("GOOSE_SPLIT_LTO");
        return e && *e && strcmp(e, "0") && (tc.name == "clang-cl" || tc.name == "clang");
    }

    bool LinkObjects(const string &unitsrsp, const vector<string> &objects,
                     const vector<string> &rsps, const string &exe, const string &builddir,
                     const string &logfile, const string &ltocache = "") const {
        vector<string> argv = { tc.cc };
        for (auto &f : CFlags()) argv.push_back(f);
        auto lto = !ltocache.empty();
        if (lto) argv.push_back("-fuse-ld=lld");
        if (tc.style == "msvc") {
            argv.push_back(cat("/Fe:", exe));
            if (opts.debug) argv.push_back(cat("/Fd:", builddir, "\\", PathStem(exe), ".pdb"));
            argv.push_back(cat("@", unitsrsp));
            for (auto &o : objects) argv.push_back(o);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            if (lto) {
                argv.push_back("/link");
                argv.push_back(cat("/lldltocache:", ltocache));
            }
        } else {
            argv.push_back(cat("@", unitsrsp));
            for (auto &o : objects) argv.push_back(o);
            argv.push_back("-o");
            argv.push_back(exe);
            for (auto &r : rsps) argv.push_back(cat("@", r));
            for (auto &l : SystemLibs()) argv.push_back(l);
            if (lto) argv.push_back(cat("-Wl,--thinlto-cache-dir=", ltocache));
        }
        return Run(argv, cat("linking ", exe), logfile);
    }
};

// Compiles the units of a split program (split.h) that have no object yet,
// side by side, under `builddir`: a unit's files are named by its name and
// a hash of its C, so an object that exists is the object of that C, and
// what no unit names any more is removed. Returns the response file naming
// every unit's object, or "" when a unit failed, after saying how.
inline string BuildSplitUnits(const Builder &b, const vector<SplitUnit> &units,
                              const string &builddir) {
    auto dir = cat(builddir, "/units/", b.tc.id, "-", b.opts.debug ? "debug" : "release",
                   b.SplitLTO() ? "-lto" : "");
    auto objext = b.tc.style == "msvc" ? ".obj" : ".o";
    struct Job {
        const SplitUnit *unit;
        string src, tmp, obj;
        vector<string> argv;
        ProcessResult r;
        string problem;
    };
    vector<Job> jobs;
    vector<string> objs;
    set<string> stems;
    std::error_code ec;
    std::unique_ptr<FileLock> lock;
    if (!b.opts.dryrun) {
        filesystem::create_directories(filesystem::path(dir), ec);
        if (ec) throw CompileError { cat("cannot create ", dir, ": ", ec.message()) };
        lock.reset(new FileLock(cat(dir, "/build.lock"),
                                "waiting for another goose building this program"));
    }
    for (auto &u : units) {
        auto stem = cat(u.name, "-", Splitter::Hex(Splitter::Hash(u.text), 16));
        auto obj = cat(dir, "/", stem, objext);
        objs.push_back(obj);
        stems.insert(stem);
        if (!b.opts.dryrun && PathExists(obj)) continue;
        Job j { &u, cat(dir, "/", stem, ".c"), cat(dir, "/", stem, ".tmp", objext), obj, { b.tc.cc }, {}, {} };
        for (auto &f : b.CFlags()) j.argv.push_back(f);
        if (b.SplitLTO()) j.argv.push_back("-flto=thin");
        if (b.tc.style == "msvc") {
            j.argv.push_back("/c");
            j.argv.push_back(j.src);
            j.argv.push_back(cat("/Fo:", j.tmp));
            if (b.opts.debug) j.argv.push_back(cat("/Fd:", obj, ".pdb"));
        } else {
            j.argv.push_back("-c");
            j.argv.push_back(j.src);
            j.argv.push_back("-o");
            j.argv.push_back(j.tmp);
        }
        jobs.push_back(std::move(j));
    }
    auto rsp = cat(dir, "/units.rsp");
    if (b.opts.dryrun || b.opts.verbose)
        for (auto &j : jobs) fprintf(b.msgs, "%s\n", CommandLineText(j.argv).c_str());
    if (b.opts.dryrun) return rsp;
    // What an earlier build left that this one has no unit for.
    for (auto &entry : filesystem::directory_iterator(filesystem::path(dir), ec)) {
        auto name = entry.path().filename().string();
        auto stem = name.substr(0, name.find('.'));
        if (stem == "build" || stem == "units" || stem == "cc" || stems.count(stem)) continue;
        std::error_code gone;
        filesystem::remove(entry.path(), gone);
    }
    if (!jobs.empty()) {
        fflush(b.msgs);
        ProgressStep step(cat("compiling ", jobs.size(), " of ", units.size(), " C units"), true);
        // The largest first: the longest compile bounds the wait.
        std::sort(jobs.begin(), jobs.end(), [](const Job &x, const Job &y) {
            return x.unit->text.size() > y.unit->text.size();
        });
        std::atomic<size_t> next { 0 };
        auto work = [&] {
            for (;;) {
                auto i = next++;
                if (i >= jobs.size()) return;
                auto &j = jobs[i];
                if (!WriteFileMakingDirs(j.src, j.unit->text)) { j.problem = cat("cannot write ", j.src); continue; }
                std::error_code e;
                filesystem::remove(filesystem::path(j.tmp), e);
                j.r = RunProcess(j.argv, "", false);
                if (!j.r.started || j.r.code) { j.problem = "failed"; continue; }
                // Only a finished compile publishes the name later builds look for.
                e = RenamePath(j.tmp, j.obj);
                if (e) j.problem = cat("cannot publish ", j.obj, ": ", e.message());
            }
        };
        vector<std::thread> pool;
        auto n = std::min<size_t>(jobs.size(), std::max(1u, std::thread::hardware_concurrency()));
        for (size_t i = 1; i < n; i++) pool.emplace_back(work);
        work();
        for (auto &t : pool) t.join();
    }
    string log;
    size_t failed = 0;
    for (auto &j : jobs) {
        if (b.opts.verbose && !j.r.output.empty()) fprintf(b.msgs, "%s", j.r.output.c_str());
        if (j.problem.empty()) continue;
        Append(log, CommandLineText(j.argv), "\n", j.r.output, "\n");
        // One unit's errors are enough to read; the log has the rest.
        if (!failed++)
            ProgressPrint(cat("compiling the C unit ", j.unit->name, " failed",
                              j.r.started ? cat(" (exit ", j.r.code, ")") : string(), ":\n    ",
                              CommandLineText(j.argv), "\n", j.problem == "failed" ? "" : j.problem,
                              b.opts.verbose ? "" : j.r.output));
    }
    if (failed) {
        WriteFileMakingDirs(cat(dir, "/cc.log"), log);
        ProgressPrint(cat(failed, " C unit(s) failed (the output is in ", dir, "/cc.log)"));
        return "";
    }
    string list;
    for (auto &o : objs) {
        auto path = o;
        for (auto &c : path) if (c == '\\') c = '/';
        Append(list, "\"", path, "\"\n");
    }
    if (!WriteFileMakingDirs(rsp, list)) throw CompileError { cat("cannot write ", rsp) };
    return rsp;
}

// The runtime object for this toolchain and configuration, compiled from
// `goose --emit-runtime`'s C on first use: GOOSE_HOME/build/runtime/<runtime
// hash>/<toolchain id>-<config>/goose_runtime.o. "" when it could not be
// built, after the builder said why.
inline string RuntimeObject(const Builder &b) {
    auto dir = cat(GooseHome(), "/build/runtime/", RuntimeVersion(), "/", b.tc.id, "-",
                   b.opts.debug ? "debug" : "release");
    auto obj = cat(dir, "/goose_runtime.", b.tc.style == "msvc" ? "obj" : "o");
    if (PathExists(obj) && !b.opts.dryrun) return obj;
    auto src = cat(dir, "/goose_runtime.c");
    auto log = cat(dir, "/cc.log");
    if (b.opts.dryrun) {
        b.CompileObject(src, obj, "compiling the runtime", log);
        return obj;
    }
    std::error_code ec;
    filesystem::create_directories(filesystem::path(dir), ec);
    if (ec) throw CompileError { cat("cannot create ", dir, ": ", ec.message()) };
    // Every first build shares these paths. Wait before writing the source
    // or invoking the compiler, then recheck what the previous builder left.
    FileLock lock(cat(dir, "/build.lock"), "waiting for another goose compiling the runtime");
    if (PathExists(obj)) return obj;
    ProgressStep step(cat("compiling the runtime for ", b.tc.desc), true);
    if (!WriteFileMakingDirs(src, RuntimeObjectSource()))
        throw CompileError { cat("cannot write ", src) };
    // A failed or interrupted compiler may leave a partial object. Only a
    // successful compile publishes the name readers use, by atomic rename.
    auto tmp = cat(dir, "/goose_runtime.tmp.", b.tc.style == "msvc" ? "obj" : "o");
    filesystem::remove(filesystem::path(tmp), ec);
    if (ec) throw CompileError { cat("cannot remove ", tmp, ": ", ec.message()) };
    if (!b.CompileObject(src, tmp, "compiling the runtime", log)) return "";
    ec = RenamePath(tmp, obj);
    if (ec) throw CompileError { cat("cannot publish ", obj, ": ", ec.message()) };
    return obj;
}

// What `goose build` does once the compiler has written the program's C to
// `cfile` in `builddir`: finds the runtime object for the toolchain `tc`,
// which built the native parts whose response files are `nativersps`
// (native.h and builtin.h, found by the caller) in link order, a package's
// before the packages it requires and the program's own C first of all, and
// links the executable, or with --shared the shared library (lib<stem>.so,
// lib<stem>.dylib or <stem>.dll), named by -o, the program's sidecar
// (`output`) or the root file. The exit code.
inline int BuildExecutable(const string &rootfile, const string &cfile, const string &builddir,
                           const Toolchain &tc, const BuildOptions &opts, FILE *msgs,
                           const vector<string> &nativersps, const string &output,
                           const vector<SplitUnit> *units = nullptr) {
    Builder b { tc, opts, msgs };
    auto t = std::chrono::steady_clock::now();
    vector<string> objects;
    if (!opts.standalone) {
        auto obj = RuntimeObject(b);
        if (obj.empty()) return 1;
        objects.push_back(obj);
    }
    auto exe = opts.exe;
    if (exe.empty()) {
        auto stem = output.empty() ? PathStem(rootfile) : output;
        if (opts.shared) {
            #ifdef _WIN32
                exe = cat(DirOf(rootfile), stem, ".dll");
            #elif defined(__APPLE__)
                exe = cat(DirOf(rootfile), "lib", stem, ".dylib");
            #else
                exe = cat(DirOf(rootfile), "lib", stem, ".so");
            #endif
        } else {
            exe = cat(DirOf(rootfile), stem);
            #ifdef _WIN32
                if (exe.size() < 4 || exe.compare(exe.size() - 4, 4, ".exe") != 0) exe += ".exe";
            #endif
        }
    }
    if (!opts.dryrun) {
        std::error_code ec;
        auto exedirpath = filesystem::path(exe).parent_path();
        if (!exedirpath.empty()) filesystem::create_directories(exedirpath, ec);
    }
    auto runtime = SecondsSince(t);
    if (units) {
        auto unitsrsp = BuildSplitUnits(b, *units, builddir);
        if (unitsrsp.empty()) return 1;
        auto compiled = SecondsSince(t);
        auto ltocache = b.SplitLTO() ? cat(builddir, "/units/lto-cache") : string();
        if (!b.LinkObjects(unitsrsp, objects, nativersps, exe, builddir, cat(builddir, "/cc.log"),
                           ltocache))
            return 1;
        if (opts.stats)
            fprintf(msgs, "time: runtime %.2f s, units %.2f s, link %.2f s\n", runtime, compiled,
                    SecondsSince(t));
    } else {
        if (!b.Link(cfile, objects, nativersps, exe, builddir, cat(builddir, "/cc.log"))) return 1;
        if (opts.stats)
            fprintf(msgs, "time: runtime %.2f s, compile and link %.2f s\n", runtime, SecondsSince(t));
    }
    if (!opts.dryrun) fprintf(msgs, "built %s\n", exe.c_str());
    return 0;
}

}  // namespace goose
