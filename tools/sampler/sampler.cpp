// A small sampling profiler for Windows: starts a program at High priority (so the session's
// background throttling does not distort it), samples every thread's stack, and prints the functions
// by exclusive and by inclusive sample count. Needs the program's .pdb beside it.
//   sampler [-ms N] [-top N] [-depth N] [-callers name] [-within name] [-o report.txt] program args...
// -within name lists, as shares of all samples, the functions running inside a function whose name
// contains `name` (it and what it calls), counted once per sample.
// -callers name also lists, for the samples inside a function whose name contains `name`, the first
// frame outside the allocator and the C runtime above it (who allocates, for -callers "operator new").
// -depth 1 samples only the running function (cheap, so -ms 1 gives many samples).
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>

static std::unordered_map<DWORD64, std::string> names;
static HANDLE proc;

// The walk reads the sampled thread's stack from one copy taken per sample, and anything else (code,
// unwind data) through a cache of pages, which images never change: a deep stack otherwise costs
// several cross-process reads per frame.
static std::vector<unsigned char> stackcopy;
static DWORD64 stacklo, stackhi;
static std::unordered_map<DWORD64, std::vector<unsigned char>> pages;

static BOOL CALLBACK ReadMem(HANDLE, DWORD64 addr, PVOID buf, DWORD size, LPDWORD read) {
    if (addr >= stacklo && addr + size <= stackhi) {
        memcpy(buf, stackcopy.data() + (addr - stacklo), size);
        if (read) *read = size;
        return TRUE;
    }
    DWORD done = 0;
    while (done < size) {
        auto at = addr + done, page = at & ~(DWORD64)4095;
        auto it = pages.find(page);
        if (it == pages.end()) {
            std::vector<unsigned char> p(4096);
            SIZE_T got = 0;
            if (!ReadProcessMemory(proc, (LPCVOID)page, p.data(), 4096, &got) || got != 4096) {
                // Not a whole readable page: read just what was asked, uncached.
                SIZE_T g = 0;
                auto ok = ReadProcessMemory(proc, (LPCVOID)at, (char *)buf + done, size - done, &g);
                if (read) *read = done + (DWORD)g;
                return ok && g > 0;
            }
            it = pages.emplace(page, std::move(p)).first;
        }
        auto off = (DWORD)(at - page), n = std::min<DWORD>(size - done, 4096 - off);
        memcpy((char *)buf + done, it->second.data() + off, n);
        done += n;
    }
    if (read) *read = size;
    return TRUE;
}

typedef LONG (NTAPI *QueryThread)(HANDLE, int, PVOID, ULONG, PULONG);

// The top of a thread's stack, from its TEB (NT_TIB::StackBase).
static DWORD64 StackBase(HANDLE th) {
    static auto query = (QueryThread)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    struct { LONG status; PVOID teb; DWORD64 cid[2]; DWORD64 mask; LONG pri, basepri; } tbi = {};
    if (!query || query(th, 0, &tbi, sizeof tbi, nullptr) != 0) return 0;
    DWORD64 base = 0;
    SIZE_T got = 0;
    if (!ReadProcessMemory(proc, (char *)tbi.teb + 8, &base, sizeof base, &got)) return 0;
    return base;
}

static const std::string &NameOf(DWORD64 addr) {
    auto it = names.find(addr);
    if (it != names.end()) return it->second;
    char buf[sizeof(SYMBOL_INFO) + 1024];
    auto si = (SYMBOL_INFO *)buf;
    si->SizeOfStruct = sizeof(SYMBOL_INFO);
    si->MaxNameLen = 1000;
    DWORD64 disp = 0;
    std::string n;
    if (SymFromAddr(proc, addr, &disp, si)) n = si->Name;
    else {
        char t[32];
        snprintf(t, sizeof t, "0x%llx", (unsigned long long)addr);
        n = t;
    }
    return names[addr] = n;
}

int main(int argc, char **argv) {
    int ms = 2, top = 60, a = 1, depth = 20000;
    const char *out = nullptr, *callers = nullptr, *within = nullptr;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-ms") && a + 1 < argc) ms = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-top") && a + 1 < argc) top = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-depth") && a + 1 < argc) depth = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-o") && a + 1 < argc) out = argv[++a];
        else if (!strcmp(argv[a], "-callers") && a + 1 < argc) callers = argv[++a];
        else if (!strcmp(argv[a], "-within") && a + 1 < argc) within = argv[++a];
        else break;
    }
    if (a >= argc) { fprintf(stderr, "usage: sampler [-ms N] [-top N] [-o report.txt] program args...\n"); return 2; }
    std::string cmd;
    for (int i = a; i < argc; i++) { cmd += '"'; cmd += argv[i]; cmd += "\" "; }
    STARTUPINFOA si = { sizeof si };
    PROCESS_INFORMATION pi;
    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, HIGH_PRIORITY_CLASS, nullptr, nullptr, &si, &pi)) {
        fprintf(stderr, "cannot start %s (%lu)\n", argv[a], GetLastError());
        return 2;
    }
    proc = pi.hProcess;
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    Sleep(50);
    SymInitialize(proc, nullptr, TRUE);
    std::vector<std::vector<DWORD64>> samples;   // each: the frames' function start addresses, innermost first
    std::unordered_map<DWORD, DWORD64> stacktops;
    auto t0 = GetTickCount64();
    // A thread snapshot lists every thread on the machine, which takes tens of milliseconds on a busy
    // one: the program's threads are listed again only every half second, and their handles kept.
    std::unordered_map<DWORD, HANDLE> threads;
    ULONGLONG listed = 0;
    while (WaitForSingleObject(proc, ms) == WAIT_TIMEOUT) {
        if (GetTickCount64() - listed > 500) {
            listed = GetTickCount64();
            HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            THREADENTRY32 te = { sizeof te };
            for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                if (te.th32OwnerProcessID != pi.dwProcessId || threads.count(te.th32ThreadID)) continue;
                HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
                if (th) threads[te.th32ThreadID] = th;
            }
            CloseHandle(snap);
        }
        for (auto it = threads.begin(); it != threads.end();) {
            auto th = it->second;
            DWORD code = 0;
            if (!GetExitCodeThread(th, &code) || code != STILL_ACTIVE) {
                CloseHandle(th);
                it = threads.erase(it);
                continue;
            }
            THREADENTRY32 te = { sizeof te };
            te.th32ThreadID = it->first;
            ++it;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &ctx)) {
                    STACKFRAME64 sf = {};
                    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                    std::vector<DWORD64> st;
                    if (depth == 1) st.push_back(ctx.Rip);
                    else {
                        auto &top = stacktops[te.th32ThreadID];
                        if (!top) top = StackBase(th);
                        stacklo = stackhi = 0;
                        if (top > ctx.Rsp && top - ctx.Rsp < (256u << 20)) {
                            stackcopy.resize((size_t)(top - ctx.Rsp));
                            SIZE_T got = 0;
                            if (ReadProcessMemory(proc, (LPCVOID)ctx.Rsp, stackcopy.data(), stackcopy.size(), &got) &&
                                got == stackcopy.size())
                                stacklo = ctx.Rsp, stackhi = top;
                        }
                    }
                    if (depth != 1) for (int d = 0; d < depth; d++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, ReadMem, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                        if (!sf.AddrPC.Offset) break;
                        st.push_back(sf.AddrPC.Offset);
                    }
                    if (!st.empty()) samples.push_back(std::move(st));
                }
                ResumeThread(th);
            }
        }
    }
    auto wall = (GetTickCount64() - t0) / 1000.0;
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    // Resolve after the run. A sample whose innermost frame is a wait is an idle thread: dropped.
    std::unordered_map<std::string, size_t> excl, incl, by, under;
    size_t used = 0;
    for (auto &st : samples) {
        auto &leaf = NameOf(st[0]);
        if (leaf.find("WaitFor") != std::string::npos || leaf.find("NtDelay") != std::string::npos || leaf.find("NtRemoveIo") != std::string::npos ||
            leaf.find("NtWaitFor") != std::string::npos || leaf.find("ZwWaitFor") != std::string::npos) continue;
        used++;
        excl[leaf]++;
        std::unordered_set<std::string> seen;
        for (auto pc : st) {
            auto &n = NameOf(pc);
            if (seen.insert(n).second) incl[n]++;
        }
        if (within) {
            // The functions called from inside the outermost frame matching, it included.
            int at = -1;
            for (int i = 0; i < (int)st.size(); i++) if (NameOf(st[i]).find(within) != std::string::npos) at = i;
            std::unordered_set<std::string> seenu;
            for (int i = 0; i <= at; i++) {
                auto &n = NameOf(st[i]);
                if (seenu.insert(n).second) under[n]++;
            }
        }
        if (callers) {
            // The first frame above the outermost one matching, skipping the allocator's own.
            int at = -1;
            for (int i = 0; i < (int)st.size(); i++) if (NameOf(st[i]).find(callers) != std::string::npos) at = i;
            if (at >= 0) {
                for (int i = at + 1; i < (int)st.size(); i++) {
                    auto &n = NameOf(st[i]);
                    if (n.find("operator new") != std::string::npos || n.find("_malloc") != std::string::npos ||
                        n.find("_free") != std::string::npos || n.find("operator delete") != std::string::npos ||
                        n.find("std::_") == 0 || n.find("std::allocator") != std::string::npos ||
                        n.find("Rtl") == 0 || n.find("_Allocate") != std::string::npos)
                        continue;
                    by[n]++;
                    break;
                }
            }
        }
    }
    FILE *f = out ? fopen(out, "w") : stdout;
    if (!f) f = stdout;
    fprintf(f, "exit %lu, wall %.1f s, %zu samples on running threads (every %d ms)\n", code, wall, used, ms);
    auto print = [&](const char *title, std::unordered_map<std::string, size_t> &m) {
        std::vector<std::pair<std::string, size_t>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](auto &x, auto &y) { return x.second > y.second; });
        fprintf(f, "\n%s\n", title);
        for (int i = 0; i < top && i < (int)v.size(); i++)
            fprintf(f, "%6.2f%%  %s\n", used ? 100.0 * v[i].second / used : 0.0, v[i].first.c_str());
    };
    print("EXCLUSIVE", excl);
    print("INCLUSIVE", incl);
    if (within) { std::string t = std::string("WITHIN ") + within; print(t.c_str(), under); }
    if (callers) { std::string t = std::string("CALLERS OF ") + callers; print(t.c_str(), by); }
    if (f != stdout) fclose(f);
    return 0;
}
