// `par for` (docs/design/child_threads.md), codegen's part: included inside
// struct CodeGen. The parser made the loop a nested function and a call to it
// over all the iterations (par_for_parse.h); that call forks here: its
// arguments go into an environment struct, and a thunk calls the function on
// each chunk of the range the pool hands it (gs_par_run), after taking the
// forking thread program's stack block and globals instance. Bodies that need
// a data stack run as the plain call until child programs (tools/par_for.md).

bool usespar = false;
int parforks = 0;

// Whether sp's call forks: a par for's function whose body runs without a
// data stack and returns nothing in flight.
bool ParForks(FnSpec *sp) {
    auto &ki = sinfo[sp];
    return sp->sf->par && !ki.needssp && !ki.hasrf && !ki.located && sp->rets.empty() &&
           sp->params.size() >= 2;
}

// The call of sp with the C arguments `args`, as a fork; false where it is
// emitted as the call.
bool EmitParFork(FnSpec *sp, const vector<string> &args) {
    if (!ParForks(sp)) return false;
    // The C parameters, one per argument, of which the last two of the
    // function's own are the range: __lo and __hi.
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
    int ilo = -1, ihi = -1;
    for (size_t i = 0; i < fields.size(); i++) {
        if (nameof(fields[i]) == lo) ilo = (int)i;
        if (nameof(fields[i]) == hi) ihi = (int)i;
    }
    if (ilo < 0 || ihi < 0) return false;
    usespar = true;
    auto k = ++parforks;
    auto env = cat("gs_parenv_", k), thunk = cat("gs_parthunk_", k);
    string decl = cat("typedef struct {\n    gs_stack *gs_stks;\n    int64_t gs_nstks;\n"
                      "    void *gs_gl;\n");
    string call, set;
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
    }
    Append(code, decl, "} ", env, ";\n\n",
           "static void ", thunk, "(void *gs_ev, int64_t gs_lo, int64_t gs_hi) {\n",
           "    ", env, " *e = (", env, " *)gs_ev;\n",
           "    gs_stks = e->gs_stks;\n",
           "    gs_nstks = e->gs_nstks;\n",
           "    gs_gl = e->gs_gl;\n",
           "    ", sinfo[sp].cname, "(", call, ");\n}\n\n");
    L(env, " ", ev, "; ", ev, ".gs_stks = gs_stks; ", ev, ".gs_nstks = gs_nstks; ", ev,
      ".gs_gl = gs_gl; ", set);
    L("gs_par(", thunk, ", &", ev, ", ", args[ilo], ", ", args[ihi], ");");
    return true;
}
