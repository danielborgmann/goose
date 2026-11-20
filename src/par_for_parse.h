// `par for` (docs/design/child_threads.md), the parser's part: included inside
// struct Parser. The loop is rewritten into a nested function over a sub-range
// of its iterations and a call to it over all of them, which is the loop's
// sequential meaning; codegen turns that call into a fork (par_for.h). The
// rewrite and its rules are in tools/par_for.md.

// `par` is a keyword only before `for`.
bool AtPar() {
    return lex.tok == T_IDENT && lex.attr == "par" && lex.PeekTok() == T_FOR;
}

// The text, parsed as a block expression in the current file with every node
// on `line`. The text stays alive as long as the views into it.
Block *ParseGenerated(const string &text, int line) {
    static std::deque<string> texts;
    auto &t = texts.emplace_back(text);
    auto saved = lex;
    lex = Lexer(saved.filename, t.c_str(), saved.fileidx);
    lex.line = lex.tokline = lex.prevendline = line;
    Block *b = nullptr;
    try {
        b = ParseBlockExpr("par for");
        if (lex.tok != T_EOF) Error("par for: the rewrite left text behind");
    } catch (...) {
        lex = saved;
        throw;
    }
    lex = saved;
    return b;
}

static bool ParIsChunk(string_view n) { return n == "chunk" || n == "chunk_at"; }

// The occurrence `id` of an enclosing variable, with its parents in `up`, is
// one of the forms that reach only this iteration's part of it: `X[idx]`,
// `X.chunk(idx, n)`, `X.chunk_at(b, idx)` or the same as plain calls, where n
// and b are names (added to `counts`) or literals.
static bool ParOwnedForm(const Ident *id, const vector<Node *> &up, string_view idx,
                         vector<string_view> &counts) {
    if (idx.empty() || up.empty()) return false;
    auto isidx = [&](const Node *n) {
        auto i = Is<Ident>(n);
        return i && i->name == idx;
    };
    auto count = [&](const Node *n) {
        if (Is<IntLit>(n)) return true;
        auto i = Is<Ident>(n);
        if (!i || i->name == idx) return false;
        counts.push_back(i->name);
        return true;
    };
    // chunk(k, n) or chunk_at(b, k) over the arguments after the array.
    auto chunkargs = [&](string_view fn, Node *a, Node *b) {
        return fn == "chunk" ? isidx(a) && count(b) : count(a) && isidx(b);
    };
    auto p = up.back();
    if (auto ix = Is<Index>(p)) return ix->obj == id && isidx(ix->idx);
    if (auto d = Is<Dot>(p)) {
        if (d->obj != id || !ParIsChunk(d->name) || up.size() < 2) return false;
        auto c = Is<Call>(up[up.size() - 2]);
        return c && c->callee == d && c->args.size() == 2 && !c->trailing &&
               chunkargs(d->name, c->args[0], c->args[1]);
    }
    if (auto c = Is<Call>(p)) {
        auto fn = Is<Ident>(c->callee);
        return fn && ParIsChunk(fn->name) && c->args.size() == 3 && c->args[0] == id &&
               !c->trailing && chunkargs(fn->name, c->args[1], c->args[2]);
    }
    return false;
}

int parcount = 0;

Node *ParsePar() {
    auto line = CurLine();
    lex.Next();
    auto f = (ForLoop *)ParseFor();
    auto pi = new ParInfo();
    pi->line = line;
    // Named by file and order, so a file parses to the same program each time.
    auto name = cat("__par", lex.fileidx, "_", ++parcount);
    auto range = Is<RangeExpr>(f->iter);
    // `&x` or an index binding: certainly over an array's elements.
    auto elems = !range && (f->byref || !f->idxvar.empty());
    auto idx = elems ? f->idxvar : f->var;
    // What the body names, and how: an enclosing variable it reaches only in
    // an owned form is passed in as the iteration's own (tools/par_for.md).
    std::map<string_view, int> uses, owneduses;
    std::set<string_view> declared;
    std::map<string_view, vector<string_view>> counts;
    vector<Node *> up;
    int nest = 0;  // Inside a nested function or a function value.
    function<void(Node *)> visit = [&](Node *n) {
        if (!n) return;
        if (auto v = Is<VarDecl>(n)) for (auto nm : v->names) declared.insert(nm);
        if (auto l = Is<ForLoop>(n)) {
            declared.insert(l->var);
            declared.insert(l->idxvar);
        }
        if (auto id = Is<Ident>(n)) {
            uses[id->name]++;
            vector<string_view> cs;
            if (!nest && ParOwnedForm(id, up, idx, cs)) {
                owneduses[id->name]++;
                counts[id->name].insert(counts[id->name].end(), cs.begin(), cs.end());
            }
        }
        auto fv = Is<FunVal>(n);
        if (fv) {
            for (auto &p : fv->params) declared.insert(p.name);
            declared.insert("it");
        }
        if (auto fd = Is<FnDecl>(n)) {
            declared.insert(fd->sf->name);
            for (auto &p : fd->sf->params) declared.insert(p.name);
            nest++;
            visit(fd->sf->body);
            nest--;
            return;
        }
        nest += fv != nullptr;
        up.push_back(n);
        n->Children(visit);
        up.pop_back();
        nest -= fv != nullptr;
    };
    visit(f->body);
    // The root variable of the array a `for &x` form walks.
    auto rootof = [](Node *e) -> const Ident * {
        for (;;) {
            if (auto d = Is<Dot>(e)) e = d->obj;
            else if (auto ix = Is<Index>(e)) e = ix->obj;
            else if (auto s = Is<SliceExpr>(e)) e = s->obj;
            else return Is<Ident>(e);
        }
    };
    auto root = elems ? rootof(f->iter) : nullptr;
    vector<string_view> owned;
    for (auto &[nm, n] : uses) {
        if (owneduses[nm] != n || declared.count(nm) || nm == f->var || nm == f->idxvar ||
            nm.find("::") != string_view::npos || nm.substr(0, 2) == "__" ||
            (root && root->name == nm))
            continue;
        owned.push_back(nm);
    }
    // `par for v in E` reads as a count or as E's elements, which E's type
    // decides; with owned arrays it is the count, the only form whose v is
    // an index.
    auto counted = !range && !elems && !owned.empty();
    auto ambiguous = !range && !elems && owned.empty();
    if (!range) {
        // E is read twice, for itself and for its length.
        function<bool(Node *)> calls = [&](Node *n) {
            if (Is<Call>(n)) return true;
            auto found = false;
            n->Children([&](Node *c) { found = found || (c && calls(c)); });
            return found;
        };
        if (!counted && calls(f->iter))
            pi->errors.push_back({ f->iter, "a par for over the result of a call: bind it to a "
                                            "variable first" });
    }
    if (root && uses.count(root->name))
        pi->errors.push_back({ f, cat("the body of a par for over &", f->var, " in ", root->name,
                                      " reaches ", root->name, " only through ", f->var) });
    string op, oa;
    for (auto nm : owned) {
        op += cat(nm, ", ");
        oa += cat(nm, "[..], ");
    }
    static const char *ints = "same<__T, i64> || same<__T, i32> || same<__T, i16> || same<__T, i8> || "
                              "same<__T, u32> || same<__T, u16> || same<__T, u8>";
    string text;
    if (range || counted) {
        text = cat("{ fn ", name, "(", op, "__lo: i64, __hi: i64) { for ", f->var,
                   " in __lo..__hi {} } ", name, "(", oa, range ? "__a, __b" : "0, __b", "); }");
    } else if (elems) {
        text = cat("{ fn ", name, "(", op, "__s, __lo: i64, __hi: i64) { for ", f->byref ? "&" : "",
                   f->var, f->idxvar.empty() ? "" : ", __j", " in __s[__lo..__hi] { ",
                   f->idxvar.empty() ? string() : cat("let ", f->idxvar, " = __lo + __j; "), "} } ",
                   name, "(", oa, "__e, 0, __e.len); }");
    } else {
        text = cat("{ fn ", name, "<__T>(__s: __T, __lo: i64, __hi: i64) { const if ", ints,
                   " { for ", f->var, " in __lo..__hi {} } else { for ", f->var,
                   " in __s[__lo..__hi] {} } } fn ", name, "c<__T>(__e: __T) -> i64 { const if ",
                   ints, " { return __e; } else { return __e.len; } } ", name, "(__e, 0, ", name,
                   "c(__e)); }");
    }
    auto gb = ParseGenerated(text, line.line);
    auto fd = Is<FnDecl>(gb->stmts[0]);
    auto call = Is<Call>(gb->stmts.back());
    assert(fd && call);
    auto sf = fd->sf;
    sf->par = pi;
    // The generated loops get the written binding's type and the body.
    vector<ForLoop *> loops;
    function<void(Node *)> findloops = [&](Node *n) {
        if (!n) return;
        if (auto l = Is<ForLoop>(n)) {
            loops.push_back(l);
            return;
        }
        n->Children(findloops);
    };
    findloops(sf->body);
    assert(loops.size() == (ambiguous ? 2 : 1));
    auto body2 = ambiguous ? (Block *)f->body->Clone(ast) : nullptr;
    for (size_t i = 0; i < loops.size(); i++) {
        auto l = loops[i];
        auto ub = i ? body2 : f->body;
        l->vartype = f->vartype;
        pi->gen.push_back(l);
        auto &pre = l->body->stmts;
        for (auto s : pre) {
            if (auto v = Is<VarDecl>(s)) v->type = f->idxtype;
            pi->gen.push_back(s);
        }
        ub->stmts.insert(ub->stmts.begin(), pre.begin(), pre.end());
        if (ub->brokenat >= 0) ub->brokenat += (int)pre.size();
        l->body = ub;
    }
    auto &args = call->args;
    auto k = owned.size();
    if (range) {
        args[k] = range->lo;
        args[k + 1] = range->hi;
    } else if (counted) {
        args[k + 1] = f->iter;
    } else if (elems) {
        args[k] = f->iter;
        Is<Dot>(args[k + 2])->obj = f->iter->Clone(ast);
    } else {
        args[0] = f->iter;
        Is<Call>(args[2])->args[0] = f->iter->Clone(ast);
    }
    pi->owned = owned;
    pi->idx = idx;
    pi->names = owned;
    pi->names.push_back(f->var);
    if (!f->idxvar.empty()) pi->names.push_back(f->idxvar);
    for (auto nm : owned) {
        for (auto c : counts[nm]) {
            if (std::find(owned.begin(), owned.end(), c) == owned.end()) pi->names.push_back(c);
        }
    }
    return gb;
}
