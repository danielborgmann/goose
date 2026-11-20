// `par for` (docs/design/child_threads.md), the checker's part: included inside
// struct TypeCheck. The parser made the loop a nested function the call to
// which forks (par_for_parse.h); these are its rules (tools/par_for.md).

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

// R1: a variable of a function around the par for whose body is being
// checked, which the body may read but not write.
bool ParCaptured(const VarDef *vd) {
    if (!vd || vd->isglobal || !vd->ownerspec || frames.empty()) return false;
    auto p = ParSpecOf(CurRealFrame().spec);
    return p && !SpecWithin(vd->ownerspec, p);
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
// write through. A `pure` marker is for later (llm_frontier §3.4).
bool ParPureExtern(const FnSpec *sp) {
    auto fi = sp->sf->line.fileidx;
    if (!ast.StdlibFile(fi)) return false;
    auto stem = PathStem(ast.sources[fi].first);
    if (stem != "std" && stem != "math" && stem != "binary") return false;
    for (auto &p : sp->sf->params)
        if (!p.type || ParWritableView(p.type)) return false;
    return true;
}

// R3: what a call or a node does besides writing places, for the message;
// empty for nothing. A user function's is its body's and its callees'.
std::map<const FnSpec *, string> pareffects;
string ParEffectOf(Node *n) {
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
    if (sp) {
        for (size_t i = 0; i < c->args.size() && i < sp->argtypes.size(); i++) {
            auto g = ParRootVar(c->args[i]);
            if (g && g->isglobal && ParWritableView(sp->argtypes[i]))
                return cat("passes the global ", g->name, " to be written");
        }
    }
    vector<FnSpec *> callees;
    if (sp) callees.push_back(sp);
    for (auto d : c->dispatch) callees.push_back(d);
    for (auto &fs : c->fmtspecs) callees.push_back(fs.second);
    for (auto k : callees) {
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
            if (!pi->errors.empty()) Error(pi->errors[0].first, pi->errors[0].second);
            for (auto sp : sf->specs) {
                if (sp->failed || !sp->body) continue;
                function<void(Node *)> walk = [&](Node *n) {
                    if (!n) return;
                    auto e = ParEffectOf(n);
                    if (!e.empty()) Error(n, cat("a par for body cannot do this: it ", e));
                    RunChildren(n, walk);
                };
                walk(sp->body);
                // The fork: the enclosing body's call of this specialization.
                function<void(Node *)> fork = [&](Node *n) {
                    if (!n) return;
                    if (auto c = Is<Call>(n); c && c->spec == sp) {
                        for (size_t i = 0; i < pi->owned.size() && i < c->args.size(); i++) {
                            auto g = ParRootVar(c->args[i]);
                            if (g && g->isglobal)
                                Error(c->args[i], cat("a par for writes only arrays of the "
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
}

// R1's message: a write the checker refused because it reaches a variable of
// the function around a par for says so, and what the body may write instead.
string ParExplain(const Node *n, const string &msg) {
    if (frames.empty() || msg.rfind("cannot", 0) != 0) return msg;
    auto p = ParSpecOf(CurRealFrame().spec);
    if (!p) return msg;
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
    if (!ParCaptured(vd)) return msg;
    auto x = vd->name;
    auto i = p->sf->par->idx.empty() ? string_view("i") : p->sf->par->idx;
    return cat("a par for body cannot write ", x, ", a variable of the function around the loop: "
               "iterations run at once, so they only read it. An array the body reaches only as ",
               x, "[", i, "], ", x, ".chunk(", i, ", n) or ", x, ".chunk_at(b, ", i, "), with n and b "
               "names it does not declare, is the iteration's own to write");
}
