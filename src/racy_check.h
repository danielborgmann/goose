// Racy views (FastGoose, tools/racy_views.md), the checker's part: included
// inside struct TypeCheck. `racy T[:]` and `racy S` are a worker's read-only
// views of main's memory. Every read through one is a copy, checked where
// its bits could break what the language assumes (a tag, a bool), so it
// yields a temporary like a call's result: nothing addresses main's memory
// through it, and nothing writes there.

// A read of a racy path is underway: Index::Check and Dot::Check set this
// for the path's base, so that its steps may cross the view. Anywhere else
// (an assignment's target, `&`, a binding by reference) a step across one is
// an error.
bool racyread = false;
struct RacyReadScope {
    TypeCheck &tc;
    bool saved;
    RacyReadScope(TypeCheck &t, bool on) : tc(t), saved(t.racyread) { tc.racyread = on; }
    ~RacyReadScope() { tc.racyread = saved; }
};

// Whether every bit pattern of t, once checked as it is read (RacyValidate
// in codegen), is a value of it, and no value of it is an address or an
// index the compiler trusts: integers, floats, bools, and fixed arrays,
// structs and fixed-mode enums of those.
bool RacyPlain(TypeExpr *t, string &why) {
    switch (t->kind) {
        case TY_INT:
            if (t->intstorage == IS_VARINT) {
                why = "varint is encoded storage";
                return false;
            }
            if (t->ix) {
                why = cat(TypeStr(t), " is an index type, which indexes without a check");
                return false;
            }
            return true;
        case TY_FLT: case TY_BOOL: return true;
        case TY_ARRAY:
            if (t->arr->akind != A_FIXED) {
                why = cat(TypeStr(t), " is not a fixed array");
                return false;
            }
            return RacyPlain(t->arr->sub, why);
        case TY_STRUCT: {
            auto inst = GetStructInst(t);
            if (inst->st->isroot) {
                why = cat(TypeStr(t), " is a root struct");
                return false;
            }
            for (auto ft : inst->ftypes)
                if (ft && !RacyPlain(ft, why)) return false;
            return true;
        }
        case TY_ENUM: {
            if (t->enu->varmode) {
                why = cat(TypeStr(t), " is in variable mode");
                return false;
            }
            auto inst = GetEnumInst(t);
            for (auto &fs : inst->vftypes)
                for (auto ft : fs)
                    if (ft && !RacyPlain(ft, why)) return false;
            return true;
        }
        case TY_VARIANT:
            return !AnyField(t, [&](TypeExpr *ft) { return !RacyPlain(ft, why); });
        case TY_REF:
            why = cat(TypeStr(t), " is a reference");
            return false;
        case TY_SLICE:
            why = cat(TypeStr(t), " is a slice");
            return false;
        default:
            why = cat(TypeStr(t), " is not plain data");
            return false;
    }
}

// The racy type's own checks, wherever one is written.
void ValidateRacy(TypeExpr *t, Line l, int pos) {
    if (pos == VT_FIELD || pos == VT_ELEM || pos == VT_GLOBAL || pos == VT_POINTEE)
        Error(l, cat("a racy view lives in a local, a parameter or a result, not in a ",
                     pos == VT_FIELD ? "field" : pos == VT_ELEM ? "array or slice"
                     : pos == VT_GLOBAL ? "global" : "reference",
                     " (tools/racy_views.md)"));
    auto v = t->sub;
    string why;
    if (v->kind == TY_SLICE) {
        ValidateType(v->sub, l, VT_ELEM);
        if (!RacyPlain(v->sub, why))
            Error(l, cat("racy views hold plain data, and ", why));
        return;
    }
    if (v->kind == TY_STRUCT) {
        ValidateType(v, l, VT_LOCAL);
        if (v->struc->st->isroot || ClassOf(v) != SC_FIXED)
            Error(l, cat("racy views of structs take fixed-size, non-root ones, not ",
                         TypeStr(v)));
        return;
    }
    Error(l, cat("racy views are of slices (racy T[:]) and structs (racy S), not ",
                 TypeStr(v)));
}

// Whether a struct field is one of those a racy struct view holds a view of
// (codegen's RacySlot): a reference to a resizable array.
static bool RacySlotType(TypeExpr *ft) {
    return ft->kind == TY_REF && ft->ref->lenstorage < 0 && !ft->ref->optional &&
           ft->ref->sub->kind == TY_ARRAY &&
           (ft->ref->sub->arr->akind == A_GROW || ft->ref->sub->arr->akind == A_GROWSHRINK);
}

// A read crossing a racy view, or an error where the path is not a read.
void RacyStep(Node *at) {
    if (!racyread)
        Error(at, "a racy view is read-only: its elements and fields can be read, never "
                  "written or referenced (tools/racy_views.md)");
}

// What a step across a racy view yields: a temporary of type t, its own.
LVal RacyTemp(TypeExpr *t) {
    LVal lv;
    lv.type = t;
    lv.Set(TempRoot(), true);
    lv.writable = false;
    lv.letbound = true;
    return lv;
}

// v[i] of a racy slice view (CheckLValue's Index step, lv its base).
LVal RacyIndex(Index *ix, const LVal &lv) {
    RacyStep(ix);
    auto elem = lv.type->sub->sub;
    {
        RacyReadScope off(*this, false);
        CheckIntAny(ix->idx);
    }
    ix->exprtype = elem;
    return RacyTemp(elem);
}

// s.f of a racy struct view: a plain field's value, or a racy view of the
// array a reference field points at.
LVal RacyField(Dot *d, TypeExpr *t) {
    RacyStep(d);
    auto st = t->sub;
    auto inst = GetStructInst(st);
    for (size_t i = 0; i < inst->st->fields.size(); i++) {
        auto &f = inst->st->fields[i];
        if (f.ispad || f.name != d->name || !inst->ftypes[i]) continue;
        auto ft = inst->ftypes[i];
        d->fieldidx = (int)i;
        string why;
        TypeExpr *rt = nullptr;
        if (RacySlotType(ft)) {
            auto elem = ft->ref->sub->arr->sub;
            if (!RacyPlain(elem, why))
                Error(d, cat("field ", f.name, " of a racy ", TypeStr(st), " cannot be read: "
                             "racy views hold plain data, and ", why));
            rt = ast.RacyOf(ast.SliceOf(elem, d->line), d->line);
        } else if (RacyPlain(ft, why)) {
            rt = ft;
        } else {
            Error(d, cat("field ", f.name, " of a racy ", TypeStr(st), " cannot be read: ",
                         why, "; a racy view reads plain fields and references to "
                         "resizable arrays"));
        }
        d->exprtype = rt;
        return RacyTemp(rt);
    }
    Error(d, cat(TypeStr(st), " has no field ", d->name));
}

// racy(x): a view of the resizable array or struct x stores (§ above).
Val CheckRacyBuiltin(Call *c, Node *arg) {
    auto av = CheckV(arg, nullptr);
    arg->exprtype = av.type;
    auto t = av.type->kind == TY_REF ? av.type->ref->sub : av.type;
    if (!av.lvalue && av.type->kind != TY_REF)
        Error(c, "racy() views a stored array or struct, not a temporary");
    TypeExpr *rt = nullptr;
    string why;
    if (t->kind == TY_ARRAY && (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK)) {
        if (!RacyPlain(t->arr->sub, why))
            Error(c, cat("racy views hold plain data, and ", why));
        rt = ast.RacyOf(ast.SliceOf(t->arr->sub, c->line), c->line);
    } else if (t->kind == TY_STRUCT) {
        if (t->struc->st->isroot || ClassOf(t) != SC_FIXED)
            Error(c, cat("racy views of structs take fixed-size, non-root ones, not ",
                         TypeStr(t)));
        rt = ast.RacyOf(t, c->line);
    } else {
        Error(c, cat("racy() takes a resizable array (T[>..] or T[>..<]) or a struct, not ",
                     TypeStr(t)));
    }
    Val v;
    v.type = rt;
    v.Set(TempRoot(), true);
    v.writable = false;
    c->rettypes.push_back(rt);
    return v;
}
