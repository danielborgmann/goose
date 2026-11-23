// `par for` (docs/design/child_threads.md), the checker's part: included inside
// struct TypeCheck. The parser made the loop a nested function the call to
// which forks (par_for_parse.h); these are its rules (tools/par_for.md).

// Storage a par for's iterations only read: a global, a variable of a
// function around the loop, or what a read-only argument of a function
// checked for them points into (VarDef::parreadonly).
bool ParReadOnlyRoot(VarDef *r) {
    return !r || r->isglobal || r->parreadonly || ParCaptured(r);
}

// A reference or slice loaded out of a field or an element in a par for's
// iterations, `path` the writability of the path to its slot. Elsewhere it is
// as writable as its slot (SlotLoadWritable), since `const` is shallow. Here
// it is only where the path is and it may point only into the iteration's
// own storage: one stored in a holder outside could point anywhere.
bool ParWritableLoad(const LVal &lv, bool path) {
    if (!path || lv.alts.empty()) return false;
    return !lv.Any([&](const RootAlt &a) { return ParReadOnlyRoot(a.root); });
}

// The innermost specialization lexically around sp, itself included, that a
// par for became; null outside one.
static FnSpec *ParSpecOf(FnSpec *sp) {
    for (; sp; sp = sp->lexparent)
        if (!sp->isfunval && sp->sf && sp->sf->par) return sp;
    return nullptr;
}

static bool SpecWithin(const FnSpec *sp, const FnSpec *outer) {
    for (; sp; sp = sp->lexparent)
        if (sp == outer) return true;
    return false;
}

// Whether the code being checked runs in a par for's iterations: its body, a
// function value called there, or a function checked for a call from one
// (FnSpec::parreach). Never under GOOSE_PAR_ALL, whose survey goes on where
// these rules would end the check.
int anypar = -1;
bool InParContext() {
    if (parall) return false;
    if (anypar < 0) {
        anypar = 0;
        for (auto sf : ast.functions) anypar |= sf->par != nullptr;
    }
    if (!anypar) return false;
    for (auto &f : frames)
        if (f.spec && (f.spec->parreach || (!f.isfunval && f.sf && f.sf->par))) return true;
    return false;
}

// A variable the body being checked may read but not write: R1's variables
// of a function around the par for, and R3's globals, there and in every
// function it calls. Either is read-only as a const is, so a reference taken
// to it is too, wherever it is passed or stored (§9.5).
bool ParCaptured(const VarDef *vd) {
    if (parall || !vd || frames.empty()) return false;
    if (vd->isglobal) return InParContext();
    if (!vd->ownerspec) return false;
    auto p = ParSpecOf(CurRealFrame().spec);
    return p && !SpecWithin(vd->ownerspec, p);
}

// GOOSE_PAR_ALL: a survey for porting much code to par for at once. The
// checker reports every refusal of R1 and R3, not the first of each loop:
// R1 is not enforced while the bodies are checked, and CheckParBodies lists
// the writes it would have refused instead (ParSurveyWrites), which is close
// to R1 but not all of it (a write through a reference taken to the variable
// is not listed). The compile fails if anything is listed.
bool parall = getenv("GOOSE_PAR_ALL") != nullptr;

// An error the check goes on past, with the calls that reached it.
void ParReport(const Node *n, const string &msg, const vector<const Call *> &via = {}) {
    Diagnostic d;
    d.where = LocOf(n);
    d.message = msg;
    for (auto c : via) d.related.push_back({ LocOf(c), "reached through this call" });
    ast.Report(d);
}

// The frame of a par for's function itself, not of a function value in it.
const ParInfo *ParFrame() {
    if (frames.empty()) return nullptr;
    auto &fr = frames.back();
    return !fr.isfunval && fr.sf ? fr.sf->par : nullptr;
}

// R4: a return out of the body, to its own function or past it.
void ParReturn(Node *r, int tf) {
    for (auto i = tf; i < (int)frames.size(); i++)
        if (!frames[i].isfunval && frames[i].sf && frames[i].sf->par)
            Error(r, "a par for body cannot return: each iteration ends at the end of the body "
                     "or at a continue");
}

// R4: a break of the marked loop, which is the rewrite's outermost one.
void ParBreak(Node *b, int si) {
    if (!ParFrame()) return;
    for (auto i = frames.back().scopebase; i < (int)scopes.size(); i++) {
        if (scopes[i].kind != SK_LOOP) continue;
        if (i == si)
            Error(b, "a par for body cannot break out of the loop: every iteration runs; "
                     "continue ends this one");
        return;
    }
}

// The body may not declare the names its disjoint writes rest on: the loop's
// own, the arrays it owns, and the counts and bounds it chunks them by.
void ParDeclare(string_view name, Line l) {
    auto pi = ParFrame();
    if (!pi || std::find(pi->names.begin(), pi->names.end(), name) == pi->names.end()) return;
    // The rewrite's own declarations: its parameters, the bindings of its loop, the only one
    // on the path to them, and the index binding `let i = __lo + __j`.
    auto loops = 0;
    auto other = false;
    for (auto &e : cur.nodepath) {
        auto n = e.node;
        if (Is<ForLoop>(n)) loops++;
        if (auto v = Is<VarDecl>(n)) {
            auto b = v->inits.size() == 1 ? Is<Binary>(v->inits[0]) : nullptr;
            auto lo = b ? Is<Ident>(b->left) : nullptr;
            if (lo && lo->name == "__lo") return;
            other = true;
        }
        other = other || Is<MatchExpr>(n) || Is<FunVal>(n) || Is<FnDecl>(n) || Is<IfExpr>(n) ||
                Is<While>(n) || Is<LoopExpr>(n);
    }
    if (loops <= 1 && !other) return;  // Its parameters have none.
    Error(l, cat("a par for body cannot declare ", name, ": the loop's disjoint writes depend on it"));
}

// The variable a written place starts at: x in x.f[i].g.
static VarDef *ParRootVar(Node *n) {
    for (;;) {
        if (auto d = Is<Dot>(n)) n = d->obj;
        else if (auto ix = Is<Index>(n)) n = ix->obj;
        else if (auto s = Is<SliceExpr>(n)) n = s->obj;
        else if (auto u = Is<Unary>(n); u && u->op == T_BITAND) n = u->child;  // `&x` (§4.1).
        else break;
    }
    auto id = Is<Ident>(n);
    return id ? id->vdef : nullptr;
}

static bool ParWritableView(const TypeExpr *t) {
    return t && (t->kind == TY_REF || t->kind == TY_SLICE || t->kind == TY_ARRAY) && !t->cq;
}

// An extern a body may call (R3): the standard library's computations on
// values, which are math's, binary's and std's taking nothing they could
// write through, and os's monotonic clock, which reads a counter each thread
// may (a reading is not ordered by iteration, as a plain for's would be).
// A `pure` marker is for later (llm_frontier §3.4).
bool ParPureExtern(const FnSpec *sp) {
    auto fi = sp->sf->line.fileidx;
    if (!ast.StdlibFile(fi)) return false;
    auto stem = PathStem(ast.sources[fi].first);
    if (stem == "os" && sp->sf->name == "clock_ns") return true;
    if (stem != "std" && stem != "math" && stem != "binary") return false;
    // A writable parameter is an out-parameter of numbers, which the call
    // must pass storage the iteration may write, as any call must (§9.5).
    auto number = [](const TypeExpr *t) {
        return t && (t->kind == TY_INT || t->kind == TY_FLT || t->kind == TY_BOOL);
    };
    for (auto &p : sp->sf->params) {
        if (!p.type) return false;
        if (!ParWritableView(p.type)) continue;
        auto t = p.type;
        auto out = (t->kind == TY_REF && !t->ref->optional && number(t->ref->sub)) ||
                   (t->kind == TY_SLICE && number(t->sub));
        if (!out) return false;
    }
    return true;
}

// R3: what a call or a node does besides writing places, for the message;
// empty for nothing. A user function's is its body's and its callees'.
std::map<const FnSpec *, string> pareffects;
static vector<FnSpec *> ParCallees(const Call *c) {
    vector<FnSpec *> callees;
    if (c->spec) callees.push_back(c->spec);
    for (auto d : c->dispatch) callees.push_back(d);
    for (auto &fs : c->fmtspecs) callees.push_back(fs.second);
    return callees;
}

// `direct`: n's own effect, not its callees'.
string ParEffectOf(Node *n, bool direct = false) {
    if (auto a = Is<Assign>(n)) {
        auto g = ParRootVar(a->lval);
        if (g && g->isglobal) return cat("writes the global ", g->name);
    }
    if (auto a = Is<IncDec>(n)) {
        auto g = ParRootVar(a->lval);
        if (g && g->isglobal) return cat("writes the global ", g->name);
    }
    auto c = Is<Call>(n);
    if (!c) return {};
    switch (c->builtin) {
        case B_PRINT: return "prints";
        case B_EXIT: return "calls exit";
        case B_THREAD_SPAWN: case B_THREAD_SPAWN_CONTAINED: return "starts a thread";
        case B_THREAD_WAIT: return "waits for a thread";
        case B_QPUT: case B_QGET: case B_QPOLL: return "uses a queue";
        case B_PAR_THREADS: case B_PAR_SPIN_US: case B_PAR_WARM:
            return cat("calls ", BuiltinByKind(c->builtin).name);
        default: break;
    }
    if (c->builtin >= 0) {
        if (BuiltinByKind(c->builtin).flags & BF_WRITE) {
            auto recv = !c->args.empty() ? c->args[0] : nullptr;
            if (auto d = Is<Dot>(c->callee)) recv = d->obj;
            auto g = recv ? ParRootVar(recv) : nullptr;
            if (g && g->isglobal) return cat("writes the global ", g->name);
        }
        return {};
    }
    auto sp = c->spec;
    if (sp && sp->sf->isextern)
        return ParPureExtern(sp) ? string() : cat("calls the extern fn ", sp->sf->qname);
    // A global passed to a writable view parameter: the checker refuses it
    // only where the callee writes it, in the callee's read-only
    // instantiation (ParReadOnlyRoot). The survey, without those rules,
    // names each such call as one that may write.
    if (sp && parall) {
        auto an = c->ValueArgNodes();  // A UFCS call's receiver included.
        for (size_t i = 0; i < an.size() && i < sp->argtypes.size(); i++) {
            auto g = ParRootVar(an[i]);
            if (g && g->isglobal && ParWritableView(sp->argtypes[i]))
                return cat("passes the global ", g->name, " to a parameter it may write");
        }
    }
    if (direct) return {};
    for (auto k : ParCallees(c)) {
        auto e = ParSpecEffect(k);
        if (!e.empty()) return cat("calls ", k->sf->qname, ", which ", e);
    }
    return {};
}

// The first effect in sp's body or its callees; a cycle adds nothing.
string ParSpecEffect(const FnSpec *sp) {
    if (!sp || !sp->body) return {};
    auto it = pareffects.find(sp);
    if (it != pareffects.end()) return it->second;
    pareffects[sp] = {};
    string found;
    function<void(Node *)> walk = [&](Node *n) {
        if (!n || !found.empty()) return;
        found = ParEffectOf(n);
        if (found.empty()) RunChildren(n, walk);
    };
    walk(sp->body);
    return pareffects[sp] = found;
}

// After every body is checked: R3 over each par for's body and callees, and
// the arrays it owns, which must be the enclosing function's own (a global
// is reachable from other code than the iteration's).
void CheckParBodies() {
    for (auto sf : ast.functions) {
        if (!sf->par) continue;
        CheckRoot([&] {
            auto pi = sf->par;
            // The first refusal ends the check of the loop; under
            // GOOSE_PAR_ALL each is reported and the check goes on.
            auto fail = [&](const Node *n, const string &msg) {
                if (!parall) Error(n, msg);
                ParReport(n, msg);
            };
            for (auto &e : pi->errors) fail(e.first, e.second);
            for (auto sp : sf->specs) {
                if (sp->failed || !sp->body) continue;
                if (parall) {
                    ParSurveyWrites(sp);
                    ParSurveyEffects(sp);
                } else {
                    function<void(Node *)> walk = [&](Node *n) {
                        if (!n) return;
                        auto e = ParEffectOf(n);
                        if (!e.empty()) Error(n, cat("a par for body cannot do this: it ", e));
                        RunChildren(n, walk);
                    };
                    walk(sp->body);
                }
                // The fork: the enclosing body's call of this specialization.
                function<void(Node *)> fork = [&](Node *n) {
                    if (!n) return;
                    if (auto c = Is<Call>(n); c && c->spec == sp) {
                        for (size_t i = 0; i < pi->owned.size() && i < c->args.size(); i++) {
                            auto g = ParRootVar(c->args[i]);
                            if (g && g->isglobal)
                                fail(c->args[i], cat("a par for writes only arrays of the "
                                                      "enclosing function: ", g->name,
                                                      " is a global"));
                        }
                    }
                    RunChildren(n, fork);
                };
                if (sp->lexparent) fork(sp->lexparent->body);
            }
        });
    }
    // The survey misses some of R1's refusals, so nothing is built under it.
    if (parall) {
        Diagnostic d;
        d.message = d.text = "GOOSE_PAR_ALL lists par for refusals as a survey and builds nothing; "
                             "unset it for the check that builds";
        ast.Report(d);
    }
}

// GOOSE_PAR_ALL's R3: each effect in the body of the par for sp and in the
// functions it calls, which are walked once each, with the calls that led
// there.
void ParSurveyEffects(FnSpec *sp) {
    set<const FnSpec *> seen { sp };
    vector<const Call *> via;
    function<void(Node *)> walk = [&](Node *n) {
        if (!n) return;
        auto e = ParEffectOf(n, true);
        if (!e.empty()) ParReport(n, cat("a par for body cannot do this: it ", e), via);
        if (auto c = Is<Call>(n)) {
            for (auto k : ParCallees(c)) {
                if (!k->body || k->sf->isextern || !seen.insert(k).second) continue;
                via.push_back(c);
                walk(k->body);
                via.pop_back();
            }
        }
        RunChildren(n, walk);
    };
    walk(sp->body);
}

// GOOSE_PAR_ALL's R1: the writes in the body of the par for sp to variables
// of the functions around it: an assignment, a builtin's write to its
// receiver, an argument for a writable view parameter, and `for &x in`.
void ParSurveyWrites(FnSpec *sp) {
    auto captured = [&](Node *place) -> VarDef * {
        auto vd = ParRootVar(place);
        if (!vd || vd->isglobal || !vd->ownerspec || SpecWithin(vd->ownerspec, sp)) return nullptr;
        return vd;
    };
    function<void(Node *)> walk = [&](Node *n) {
        if (!n) return;
        VarDef *vd = nullptr;
        if (auto as = Is<Assign>(n)) vd = captured(as->lval);
        else if (auto id = Is<IncDec>(n)) vd = captured(id->lval);
        else if (auto l = Is<ForLoop>(n); l && l->byref) vd = captured(l->iter);
        else if (auto c = Is<Call>(n)) {
            auto an = c->ValueArgNodes();
            if (c->builtin >= 0) {
                if ((BuiltinByKind(c->builtin).flags & BF_WRITE) && !an.empty()) vd = captured(an[0]);
            } else if (c->spec) {
                for (size_t i = 0; !vd && i < an.size() && i < c->spec->argtypes.size(); i++)
                    if (ParWritableView(c->spec->argtypes[i])) vd = captured(an[i]);
            }
        }
        if (vd)
            ParReport(n, cat("a par for body cannot write ", vd->name, ", a variable of the "
                             "function around the loop"));
        RunChildren(n, walk);
    };
    walk(sp->body);
}

// The message of a write the checker refused in a par for's iterations: one
// that reaches a global, or a variable of the function around the loop (and
// what the body may write instead), says so, and one through a reference
// that is read-only says why it may be.
string ParExplain(const Node *n, const string &msg) {
    if (frames.empty() || !InParContext()) return msg;
    auto readonly = [&] {
        if (msg.find("§9.5") == string::npos) return msg;  // Not a writability rule's.
        return cat(msg, ". In a par for and the functions it calls, globals and the variables of "
                   "the function around the loop are read-only, as is every reference to them "
                   "or stored in them");
    };
    if (msg.rfind("cannot", 0) != 0) return readonly();
    auto p = ParSpecOf(CurRealFrame().spec);  // Null in a function the body calls.
    // The written place's root, through a call's receiver and through a
    // `for &v in E` binding to E's.
    std::function<VarDef *(Node *)> root = [&](Node *e) -> VarDef * {
        if (auto a = Is<Assign>(e)) return root(a->lval);
        if (auto a = Is<IncDec>(e)) return root(a->lval);
        if (auto c = Is<Call>(e)) {
            if (auto d = Is<Dot>(c->callee)) return root(d->obj);
            return c->args.empty() ? nullptr : root(c->args[0]);
        }
        auto vd = ParRootVar(e);
        if (!vd || ParCaptured(vd)) return vd;
        for (auto i = cur.nodepath.rbegin(); i != cur.nodepath.rend(); ++i)
            if (auto l = Is<ForLoop>(i->node); l && l->byref && l->var == vd->name)
                return root(l->iter);
        return vd;
    };
    auto vd = root((Node *)n);
    if (vd && vd->isglobal && ParCaptured(vd))
        return cat("a par for body cannot write the global ", vd->name, ", itself or in a function "
                   "it calls: iterations run at once, so they only read globals");
    if (!p || !ParCaptured(vd)) return readonly();
    auto x = vd->name;
    auto i = p->sf->par->idx.empty() ? string_view("i") : p->sf->par->idx;
    return cat("a par for body cannot write ", x, ", a variable of the function around the loop: "
               "iterations run at once, so they only read it. An array the body reaches only as ",
               x, "[", i, "], ", x, ".chunk(", i, ", n) or ", x, ".chunk_at(b, ", i, "), with n and b "
               "names it does not declare, is the iteration's own to write; one it reaches only "
               "through ", x, ".atomic_or, ", x, ".atomic_add and ", x, ".atomic_load is shared, "
               "a 64-bit word at a time");
}
