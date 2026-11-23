// Racy views (FastGoose, tools/racy_views.md), codegen's part: included
// inside struct CodeGen. A `racy T[:]` is the runtime's gs_racy (base, the
// address of main's length, and the most elements the region from base
// holds); a `racy S` is S's address and a gs_racy for each of S's references
// to resizable arrays, all taken by racy() on main. A read through one is a
// copy made of single volatile loads (gs_racy_copy), checked against the
// length it loaded and then for the bits the language assumes of its type.

bool usesracy = false;

// Whether a field of a struct is one a racy view of the struct holds a view
// of (the checker's RacySlotType).
static bool RacySlot(TypeExpr *ft) {
    return ft && ft->kind == TY_REF && ft->ref->lenstorage < 0 && !ft->ref->optional &&
           ft->ref->sub->kind == TY_ARRAY &&
           (ft->ref->sub->arr->akind == A_GROW || ft->ref->sub->arr->akind == A_GROWSHRINK);
}

// The fields of struct type st that have views, in order.
vector<size_t> RacySlots(TypeExpr *st) {
    vector<size_t> out;
    auto si = SI(st);
    for (size_t i = 0; i < si->ftypes.size(); i++)
        if (!si->st->fields[i].ispad && RacySlot(si->ftypes[i])) out.push_back(i);
    return out;
}

int64_t RacySize(TypeExpr *t) {
    if (t->sub->kind == TY_SLICE) return 24;
    return 8 + 24 * (int64_t)std::max<size_t>(1, RacySlots(t->sub).size());
}

string RacyCT(TypeExpr *t) {
    usesracy = true;
    if (t->sub->kind == TY_SLICE) return "gs_racy";
    auto m = Mangle(t);
    auto it = ctypes.find(m);
    if (it != ctypes.end()) return it->second;
    auto name = Unique(m);
    ctypes[m] = name;
    Append(tdecls, "typedef struct { const uint8_t *p; gs_racy v[",
           std::max<size_t>(1, RacySlots(t->sub).size()), "]; } ", name, ";\n\n");
    return name;
}

// Whether a value of t read from racy memory needs a check of its bits.
bool RacyNeedsCheck(TypeExpr *t) {
    switch (t->kind) {
        case TY_BOOL: case TY_ENUM: return true;
        case TY_ARRAY: return RacyNeedsCheck(t->arr->sub);
        case TY_STRUCT: case TY_VARIANT: {
            auto any = false;
            EachField(t, [&](TypeExpr *ft) { any = any || RacyNeedsCheck(ft); });
            return any;
        }
        default: return false;
    }
}

// The checks of a value x of type t just copied from racy memory: a bool
// becomes 0 or 1, and an enum's tag must name a variant, whose payload is
// then checked in turn. A tag no variant has would reach a match's
// unreachable arm, which the C compiler may assume away.
void RacyCheck(TypeExpr *t, const string &x, Line ln) {
    if (!RacyNeedsCheck(t)) return;
    switch (t->kind) {
        case TY_BOOL: L(x, " = ", x, " != 0;"); return;
        case TY_ARRAY: {
            auto j = T();
            L("for (int64_t ", j, " = 0; ", j, " < ", ArrSize(t->arr), "; ", j, "++) {");
            RacyCheck(t->arr->sub, cat(x, ".e[", j, "]"), ln);
            L("}");
            return;
        }
        case TY_STRUCT: {
            auto si = SI(t);
            for (size_t i = 0; i < si->ftypes.size(); i++)
                if (!si->st->fields[i].ispad && si->ftypes[i])
                    RacyCheck(si->ftypes[i], cat(x, ".", Sanitize(si->st->fields[i].name)), ln);
            return;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            auto vi = ei->en->VariantIndex(t->var->variant);
            auto &v = ei->en->variants[vi];
            for (size_t i = 0; i < v.fields.size(); i++)
                if (!v.fields[i].ispad && ei->vftypes[vi][i])
                    RacyCheck(ei->vftypes[vi][i], cat(x, ".", Sanitize(v.fields[i].name)), ln);
            return;
        }
        case TY_ENUM: {
            auto ei = EIOf(t);
            L("switch (", x, ".tag) {");
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                L("case ", TagConst(ei, (int)vi), ":");
                if (!EmptyLayout(ei->en->variants[vi].fields))
                    RacyCheck(VariantType(t, (int)vi),
                              cat(x, ".u.v_", Sanitize(ei->en->variants[vi].name)), ln);
                L("    break;");
            }
            L("default: gs_abort(GS_E_TAG, ", LocArgs(ln), ");");
            L("}");
            return;
        }
        default: return;
    }
}

// A copy of a value of type t at racy address `src`, checked.
Loc RacyLoad(TypeExpr *t, const string &src, Line ln) {
    auto e = T();
    L(CT(t), " ", e, ";");
    L("gs_racy_copy(&", e, ", ", src, ", ", FixedSize(t), ");");
    RacyCheck(t, e, ln);
    Loc r;
    r.s = e;
    r.t = t;
    r.val = true;
    return r;
}

// v[i] of a racy slice view at lv.
Loc RacyIndexLoc(const Loc &lv, Index *ix) {
    auto elem = lv.t->sub->sub;
    auto v = T(), i = T();
    L("gs_racy ", v, " = ", lv.s, ";");
    L("int64_t ", i, " = ", GenX(ix->idx), ";");
    L("(void)GS_IDX(", i, ", gs_racy_len(", v, "), ", LocArgs(ix->line), ");");
    return RacyLoad(elem, cat(v, ".base + ", i, " * ", FixedSize(elem)), ix->line);
}

// s.f of a racy struct view at lv: a reference field's view, or a copy of
// a plain field.
Loc RacyFieldLoc(const Loc &lv, Dot *d) {
    auto st = lv.t->sub;
    auto si = SI(st);
    auto fi = (size_t)d->fieldidx;
    auto ft = si->ftypes[fi];
    if (RacySlot(ft)) {
        auto slots = RacySlots(st);
        auto k = find(slots.begin(), slots.end(), fi) - slots.begin();
        Loc r;
        r.s = cat(lv.s, ".v[", k, "]");
        r.t = d->exprtype;
        r.val = true;
        return r;
    }
    return RacyLoad(ft, cat(lv.s, ".p + offsetof(", CT(st), ", ",
                            Sanitize(si->st->fields[fi].name), ")"), d->line);
}

// racy(x), on main: the view of x's array, or of x's struct and its arrays.
string EmitRacyMake(Call *c, Node *arg) {
    usesracy = true;
    auto lv = GenLoc(arg);
    while (lv.t->kind == TY_REF) DerefLoc(lv);
    auto rt = c->exprtype;
    if (rt->sub->kind == TY_SLICE) {
        auto v = T();
        L("gs_racy ", v, " = gs_racy_make((const uint8_t *)(", lv.s, "), &(", lv.lenlv, "), ",
          FixedSize(rt->sub->sub), ");");
        return v;
    }
    auto st = rt->sub;
    auto si = SI(st);
    auto v = T();
    L(RacyCT(rt), " ", v, ";");
    L(v, ".p = (const uint8_t *)&(", lv.s, ");");
    auto slots = RacySlots(st);
    if (slots.empty()) L(v, ".v[0] = gs_racy_make(NULL, NULL, 0);");
    for (size_t k = 0; k < slots.size(); k++) {
        auto fi = slots[k];
        auto f = cat(lv.s, ".", Sanitize(si->st->fields[fi].name));
        auto elem = si->ftypes[fi]->ref->sub->arr->sub;
        L(v, ".v[", k, "] = gs_racy_make((const uint8_t *)", f, ".hdr->base, &", f,
          ".hdr->len, ", FixedSize(elem), ");");
    }
    return v;
}
