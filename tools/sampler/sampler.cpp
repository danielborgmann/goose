// A small sampling profiler for Windows: starts a program at High priority (so the session's
// background throttling does not distort it), samples every thread's stack, and prints the functions
// by exclusive and by inclusive sample count. Needs the program's .pdb beside it.
//   sampler [-ms N] [-top N] [-o report.txt] program args...
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
    int ms = 2, top = 60, a = 1;
    const char *out = nullptr;
    for (; a < argc && argv[a][0] == '-'; a++) {
        if (!strcmp(argv[a], "-ms") && a + 1 < argc) ms = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-top") && a + 1 < argc) top = atoi(argv[++a]);
        else if (!strcmp(argv[a], "-o") && a + 1 < argc) out = argv[++a];
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
    std::unordered_map<DWORD64, DWORD64> fnstart;
    auto t0 = GetTickCount64();
    while (WaitForSingleObject(proc, ms) == WAIT_TIMEOUT) {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        THREADENTRY32 te = { sizeof te };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pi.dwProcessId) continue;
            HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, te.th32ThreadID);
            if (!th) continue;
            if (SuspendThread(th) != (DWORD)-1) {
                CONTEXT ctx = {};
                ctx.ContextFlags = CONTEXT_FULL;
                if (GetThreadContext(th, &ctx)) {
                    STACKFRAME64 sf = {};
                    sf.AddrPC.Offset = ctx.Rip; sf.AddrPC.Mode = AddrModeFlat;
                    sf.AddrFrame.Offset = ctx.Rbp; sf.AddrFrame.Mode = AddrModeFlat;
                    sf.AddrStack.Offset = ctx.Rsp; sf.AddrStack.Mode = AddrModeFlat;
                    std::vector<DWORD64> st;
                    for (int d = 0; d < 20000; d++) {
                        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, proc, th, &sf, &ctx, nullptr, SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) break;
                        if (!sf.AddrPC.Offset) break;
                        st.push_back(sf.AddrPC.Offset);
                    }
                    if (!st.empty()) samples.push_back(std::move(st));
                }
                ResumeThread(th);
            }
            CloseHandle(th);
        }
        CloseHandle(snap);
    }
    auto wall = (GetTickCount64() - t0) / 1000.0;
    DWORD code = 0;
    GetExitCodeProcess(proc, &code);
    // Resolve after the run. A sample whose innermost frame is a wait is an idle thread: dropped.
    std::unordered_map<std::string, size_t> excl, incl;
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
    if (f != stdout) fclose(f);
    return 0;
}
