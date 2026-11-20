// `par for` (docs/design/child_threads.md), codegen's part: included inside
// struct CodeGen. The parser made the loop a nested function and a call to it
// over all the iterations (par_for_parse.h); that call forks here: its
// arguments go into an environment struct, and a thunk calls the function on
// each chunk of the range the pool hands it (gs_par_run), after taking the
// forking thread program's stack block and globals instance. A body that
// needs data stacks runs on a pool thread on that thread's own block, rewound
// after each chunk, and on the forking thread above the fork's stacks.

bool usespar = false;
int parforks = 0;
// The thunks of the forks whose bodies need data stacks, written once the
// stack counts are known (EmitParThunks): the call on the forking thread and
// the call on a pool thread.
struct ParThunk {
    FnSpec *sp;
    string name, env, forking, pooled;
};
vector<ParThunk> parthunks;

// Whether sp's call forks: a par for's function that returns nothing in
// flight.
bool ParForks(FnSpec *sp) {
    auto &ki = sinfo[sp];
    return sp->sf->par && !ki.hasrf && !ki.located && sp->rets.empty() && sp->params.size() >= 2;
}

// The call of sp with the C arguments `args`, as a fork; false where it is
// emitted as the call.
bool EmitParFork(FnSpec *sp, const vector<string> &args) {
    if (!ParForks(sp)) return false;
    // The C parameters, one per argument, of which the last two of the
    // function's own are the range, __lo and __hi, and gs_sp the stack index
    // its stacks start at.
    auto sig = SigParams(sp, false);
    vector<string> fields;
    for (size_t b = 0;;) {
        auto e = sig.find(", ", b);
        fields.push_back(sig.substr(b, e == string::npos ? e : e - b));
        if (e == string::npos) break;
        b = e + 2;
    }
    if (fields.size() != args.size()) return false;
    auto nameof = [](const string &d) { return d.substr(d.find_last_of(" *") + 1); };
    auto n = sp->params.size();
    auto lo = cat("p", n - 2), hi = cat("p", n - 1);
    int ilo = -1, ihi = -1, isp = -1;
    for (size_t i = 0; i < fields.size(); i++) {
        if (nameof(fields[i]) == lo) ilo = (int)i;
        if (nameof(fields[i]) == hi) ihi = (int)i;
        if (nameof(fields[i]) == "gs_sp") isp = (int)i;
    }
    auto stacks = sinfo[sp].needssp;
    if (ilo < 0 || ihi < 0 || stacks != (isp >= 0)) return false;
    usespar = true;
    auto k = ++parforks;
    auto env = cat("gs_parenv_", k), thunk = cat("gs_parthunk_", k);
    string decl = cat("typedef struct {\n    gs_stack *gs_stks;\n    int64_t gs_nstks;\n"
                      "    void *gs_gl;\n");
    string call, pooled, set;
    auto ev = T();
    for (size_t i = 0; i < fields.size(); i++) {
        string a;
        if ((int)i == ilo) a = "gs_lo";
        else if ((int)i == ihi) a = "gs_hi";
        else {
            auto f = nameof(fields[i]);
            Append(decl, "    ", fields[i], ";\n");
            Append(set, ev, ".", f, " = ", args[i], "; ");
            a = cat("e->", f);
        }
        Append(call, i ? ", " : "", a);
        Append(pooled, i ? ", " : "", (int)i == isp ? "0" : a);
    }
    Append(code, decl, "} ", env, ";\n\n");
    auto &cname = sinfo[sp].cname;
    if (stacks) {
        Append(protos, "static void ", thunk, "(void *gs_ev, int64_t gs_lo, int64_t gs_hi);\n");
        parthunks.push_back({ sp, thunk, env, cat(cname, "(", call, ")"),
                              cat(cname, "(", pooled, ")") });
    } else {
        Append(code, "static void ", thunk, "(void *gs_ev, int64_t gs_lo, int64_t gs_hi) {\n",
               "    ", env, " *e = (", env, " *)gs_ev;\n",
               "    gs_stks = e->gs_stks;\n",
               "    gs_nstks = e->gs_nstks;\n",
               "    gs_gl = e->gs_gl;\n",
               "    ", cname, "(", call, ");\n}\n\n");
    }
    L(env, " ", ev, "; ", ev, ".gs_stks = gs_stks; ", ev, ".gs_nstks = gs_nstks; ", ev,
      ".gs_gl = gs_gl; ", set);
    L("gs_par(", thunk, ", &", ev, ", ", args[ilo], ", ", args[ihi], ");");
    return true;
}

// The thunks of the forks whose bodies need data stacks, once every count is
// known: on the forking thread the body runs above the fork's stacks; on a
// pool thread on the thread's block, of the most stacks any such body needs,
// which is rewound after each chunk. That block counts as a worker's regions.
void EmitParThunks(int64_t &workerregions) {
    if (parthunks.empty()) return;
    int64_t n = 1;
    for (auto &t : parthunks) n = std::max(n, stackneed[t.sp]);
    CheckStackLimit("a par for body", n, parthunks[0].sp->sf->line);
    workerregions = std::max(workerregions, n);
    for (auto &t : parthunks) {
        Append(code, "static void ", t.name, "(void *gs_ev, int64_t gs_lo, int64_t gs_hi) {\n",
               "    ", t.env, " *e = (", t.env, " *)gs_ev;\n",
               "    if (!gs_par_pooled_here()) {\n",
               "        ", t.forking, ";\n",
               "        return;\n",
               "    }\n",
               "    gs_par_block(", n, ");\n",
               "    gs_gl = e->gs_gl;\n",
               "    uint8_t *gs_tops[", n, "];\n",
               "    for (int64_t i = 0; i < ", n, "; i++) gs_tops[i] = gs_stks[i].top;\n",
               "    ", t.pooled, ";\n",
               "    for (int64_t i = 0; i < ", n, "; i++) gs_stks[i].top = gs_tops[i];\n",
               "}\n\n");
    }
}
