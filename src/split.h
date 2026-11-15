// Goose compiler — split output (`goose build --split`): the program's C as
// many small units instead of one file, so that a C compiler builds them in
// parallel and, after an edit, only those whose text changed.
//
// This works on the text codegen wrote, section by section, and changes
// nothing in how that text is made. Each top-level item (a type, a data
// object, a prototype, a function) is found by its line structure and named
// by its declarator. A function or data object is defined in one unit,
// chosen by a hash of its name so that an edit elsewhere does not move it,
// and gets external linkage; a unit holds the runtime's header, then only
// the items its definitions name (types whole, other units' functions and
// data as declarations), then its definitions. A GS_INLINE function stays
// static and is repeated in every unit that calls it.
#pragma once

namespace goose {

struct SplitUnit {
    string name;   // A C identifier: "main", "u0012", "big_<function>".
    string text;
};

struct Splitter {
    enum Kind { TYPE, MACRO, PROTO, INLINE, FN, DATA };
    struct Item {
        Kind kind = TYPE;
        bool packed = false;   // Emitted inside #pragma pack(1), as codegen had it.
        bool anontype = false; // DATA whose declaration cannot precede its definition.
        string text;           // TYPE, MACRO, PROTO, INLINE: what a unit needing it holds.
        string decl, defn;     // INLINE, FN, DATA: the declaration, and the definition in its unit.
        vector<string> names;
        vector<int> deps;      // Name ids the whole item mentions.
        vector<int> decldeps;  // Name ids `decl` mentions.
        string unit;           // FN, DATA: the unit it goes in, when not by its name.
    };
    vector<Item> items;
    unordered_map<string_view, int> nameids;
    vector<vector<int>> byname;   // Name id -> the items declaring it.

    static bool IdStart(char c) { return isalpha((unsigned char)c) || c == '_'; }
    static bool IdChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

    // Calls f(begin, end) for each identifier of `t` outside string and
    // character literals and comments.
    template<typename F> static void EachIdent(string_view t, F f) {
        for (size_t i = 0; i < t.size();) {
            auto c = t[i];
            if (c == '"' || c == '\'') {
                for (i++; i < t.size() && t[i] != c; i++) if (t[i] == '\\') i++;
                i++;
            } else if (c == '/' && i + 1 < t.size() && t[i + 1] == '*') {
                auto e = t.find("*/", i + 2);
                i = e == string_view::npos ? t.size() : e + 2;
            } else if (c == '/' && i + 1 < t.size() && t[i + 1] == '/') {
                auto e = t.find('\n', i);
                i = e == string_view::npos ? t.size() : e;
            } else if (IdStart(c)) {
                auto b = i;
                while (i < t.size() && IdChar(t[i])) i++;
                f(b, i);
            } else if (isdigit((unsigned char)c)) {
                // A number's suffix or exponent is not an identifier.
                while (i < t.size() && (IdChar(t[i]) || t[i] == '.')) i++;
            } else {
                i++;
            }
        }
    }

    // The brace depth a line adds, literals and comments apart.
    static int Braces(string_view line) {
        int d = 0;
        for (size_t i = 0; i < line.size(); i++) {
            auto c = line[i];
            if (c == '"' || c == '\'') {
                for (i++; i < line.size() && line[i] != c; i++) if (line[i] == '\\') i++;
            } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '*') {
                auto e = line.find("*/", i + 2);
                if (e == string_view::npos) break;
                i = e + 1;
            } else if (c == '/' && i + 1 < line.size() && line[i + 1] == '/') {
                break;
            } else if (c == '{') {
                d++;
            } else if (c == '}') {
                d--;
            }
        }
        return d;
    }

    static bool Starts(string_view s, string_view p) { return s.substr(0, p.size()) == p; }
    static bool Ends(string_view s, string_view p) {
        return s.size() >= p.size() && s.substr(s.size() - p.size()) == p;
    }
    static string_view RTrim(string_view s) {
        while (!s.empty() && isspace((unsigned char)s.back())) s.remove_suffix(1);
        return s;
    }

    // The name a declaration without its initializer declares: the one in
    // `(*name)` for a pointer to a function or an array, else the last
    // identifier before any array bounds, attribute macros (GS_ALIGN1) apart.
    static string DeclName(string_view d) {
        d = RTrim(d);
        if (!d.empty() && d.back() == ';') d = RTrim(d.substr(0, d.size() - 1));
        for (auto p = d.find("(*"); p != string_view::npos; p = d.find("(*", p + 1)) {
            auto b = p + 2;
            while (b < d.size() && d[b] == ' ') b++;
            auto e = b;
            while (e < d.size() && IdChar(d[e])) e++;
            if (e > b && e < d.size() && (d[e] == ')' || d[e] == '[')) return string(d.substr(b, e - b));
        }
        while (!d.empty() && d.back() == ']') {
            auto b = d.rfind('[');
            if (b == string_view::npos) break;
            d = RTrim(d.substr(0, b));
        }
        string last;
        EachIdent(d, [&](size_t b, size_t e) {
            auto w = d.substr(b, e - b);
            auto attr = Starts(w, "GS_");
            for (auto c : w) if (islower((unsigned char)c)) attr = false;
            if (!attr) last = string(w);
        });
        return last;
    }

    // The function a line declares or starts defining: the first identifier
    // a `(` follows with no `;`, `=` or `{` before it. "" when there is none.
    static string FnName(string_view line) {
        string name;
        size_t stop = line.find_first_of(";={");
        EachIdent(line, [&](size_t b, size_t e) {
            if (name.empty() && b < stop && e < line.size() && line[e] == '(')
                name = string(line.substr(b, e - b));
        });
        return name;
    }

    void Classify(string_view text, bool packed) {
        Item it;
        it.packed = packed;
        // A conditional block in front of an item (a simd function's
        // versions) stays with it, in its unit.
        size_t head = 0;
        while (Starts(text.substr(head), "#if")) {
            int pp = 0;
            for (size_t b = head; b < text.size();) {
                auto e = text.find('\n', b);
                e = e == string_view::npos ? text.size() : e + 1;
                auto line = text.substr(b, e - b);
                if (Starts(line, "#if")) pp++;
                else if (Starts(line, "#endif")) pp--;
                b = e;
                if (!pp || b >= text.size()) { head = b; break; }
            }
        }
        auto prefix = text.substr(0, head);
        auto body = text.substr(head);
        auto nl = body.find('\n');
        auto first = body.substr(0, nl == string_view::npos ? body.size() : nl);
        auto rest = nl == string_view::npos ? string_view() : body.substr(nl + 1);
        auto whole = RTrim(body);
        if (Starts(body, "#define ")) {
            it.kind = MACRO;
            auto b = body.substr(8);
            size_t e = 0;
            while (e < b.size() && IdChar(b[e])) e++;
            it.names.push_back(string(b.substr(0, e)));
            it.text = string(text);
        } else if (Starts(body, "#")) {
            return;   // #undef and the like between items: nothing a unit needs.
        } else if (Starts(body, "typedef") || Starts(body, "struct") || Starts(body, "enum") ||
                   Starts(body, "union")) {
            it.kind = TYPE;
            it.text = string(text);
            auto istypedef = Starts(body, "typedef");
            auto tagged = istypedef ? body.substr(7) : body;
            while (!tagged.empty() && tagged[0] == ' ') tagged.remove_prefix(1);
            auto isenum = Starts(tagged, "enum");
            for (auto kw : { "struct ", "enum ", "union " }) {
                if (!Starts(tagged, kw)) continue;
                auto t = tagged.substr(strlen(kw));
                size_t e = 0;
                while (e < t.size() && IdChar(t[e])) e++;
                if (e) it.names.push_back(string(t.substr(0, e)));
            }
            if (istypedef) {
                auto d = whole;
                if (!d.empty() && d.back() == ';') d = RTrim(d.substr(0, d.size() - 1));
                // The declarator follows the closing brace of a struct body.
                auto close = d.rfind('}');
                auto decl = close == string_view::npos ? d : d.substr(close + 1);
                string name;
                if (!decl.empty() && decl.back() == ')') {
                    // A function pointer type: typedef ret (*name)(params).
                    auto p = d.find("(*");
                    if (p != string_view::npos) {
                        auto b = p + 2;
                        while (b < d.size() && d[b] == ' ') b++;
                        auto e = b;
                        while (e < d.size() && IdChar(d[e])) e++;
                        name = string(d.substr(b, e - b));
                    }
                } else {
                    name = DeclName(decl);
                }
                if (!name.empty()) it.names.push_back(name);
            }
            if (isenum) {
                auto open = body.find('{'), close = body.rfind('}');
                if (open != string_view::npos && close != string_view::npos) {
                    auto inner = body.substr(open + 1, close - open - 1);
                    for (size_t b = 0; b <= inner.size();) {
                        auto e = std::min(inner.find(',', b), inner.size());
                        auto part = inner.substr(b, e - b);
                        size_t s = 0;
                        while (s < part.size() && isspace((unsigned char)part[s])) s++;
                        auto n = s;
                        while (n < part.size() && IdChar(part[n])) n++;
                        if (n > s) it.names.push_back(string(part.substr(s, n - s)));
                        b = e + 1;
                    }
                }
            }
        } else {
            auto name = FnName(first);
            auto isdef = !whole.empty() && whole.back() == '}';
            auto isfn = !name.empty() &&
                        (isdef || (Ends(whole, ");") && first.find('=') == string_view::npos));
            if (isfn) {
                it.names.push_back(name);
                auto h = first;
                if (Starts(h, "static ")) h.remove_prefix(7);
                auto inl = Starts(h, "GS_INLINE ");
                if (isdef && inl) {
                    // Every caller needs the body, so every unit that calls
                    // it holds it, static as it was.
                    it.kind = INLINE;
                    auto brace = first.rfind('{');
                    it.decl = cat(RTrim(first.substr(0, brace)), ";\n");
                    it.defn = string(text);
                } else if (isdef) {
                    it.kind = FN;
                    auto brace = h.rfind('{');
                    it.decl = cat(RTrim(h.substr(0, brace)), ";\n");
                    it.defn = cat(prefix, h, "\n", rest);
                } else {
                    it.kind = PROTO;
                    if (inl) h.remove_prefix(10);
                    it.text = cat(h, "\n", rest);
                }
            } else {
                it.kind = DATA;
                auto b = body;
                if (Starts(b, "static ")) b.remove_prefix(7);
                // The initializer starts at the first " = " outside braces
                // (the type may be a struct spelled out in place).
                auto eq = string_view::npos;
                for (size_t i = 0, depth = 0; i + 2 < b.size(); i++) {
                    if (b[i] == '{') depth++;
                    else if (b[i] == '}') depth--;
                    else if (!depth && b[i] == ' ' && b[i + 1] == '=' && b[i + 2] == ' ') { eq = i; break; }
                }
                auto d = RTrim(eq != string_view::npos ? b.substr(0, eq) : b);
                if (!d.empty() && d.back() == ';') d = RTrim(d.substr(0, d.size() - 1));
                it.names.push_back(DeclName(d));
                it.decl = cat("extern ", d, ";\n");
                it.defn = cat(prefix, b);
                // A struct spelled out in place is a new type each time, so
                // the unit defining the object cannot also declare it.
                it.anontype = d.find('{') != string_view::npos;
            }
        }
        if (it.names.empty() || it.names[0].empty())
            throw CompileError { cat("--split cannot name this item of the generated C:\n",
                                     text.substr(0, 200)) };
        items.push_back(std::move(it));
    }

    // Top-level items of a section: text up to a line that ends one (`}` or
    // a `;` at brace depth 0), with a preprocessor line between items an
    // item of its own and a conditional block part of the item after it.
    void Parse(string_view t, bool packed) {
        size_t start = string_view::npos;
        int depth = 0, pp = 0;
        for (size_t b = 0; b < t.size();) {
            auto e = t.find('\n', b);
            auto next = e == string_view::npos ? t.size() : e + 1;
            auto line = t.substr(b, (e == string_view::npos ? t.size() : e) - b);
            auto blank = line.find_first_not_of(" \t\r") == string_view::npos;
            auto fresh = start == string_view::npos;
            if (fresh && blank) { b = next; continue; }
            if (fresh) start = b;
            if (!line.empty() && line[0] == '#') {
                if (Starts(line, "#if")) pp++;
                else if (Starts(line, "#endif")) pp--;
                // A lone directive is an item; one inside a body or closing
                // a leading block is part of what it sits in.
                if (fresh && !pp) { Classify(t.substr(start, next - start), packed); start = string_view::npos; }
                b = next;
                continue;
            }
            depth += Braces(line);
            if (!depth && !pp && !blank && (line.back() == ';' || line == "}")) {
                Classify(t.substr(start, next - start), packed);
                start = string_view::npos;
            }
            b = next;
        }
        if (start != string_view::npos && t.substr(start).find_first_not_of(" \t\r\n") != string_view::npos)
            throw CompileError { cat("--split found no end to this item of the generated C:\n",
                                     t.substr(start, 200)) };
    }

    static uint64_t Hash(string_view s, uint64_t h = 14695981039346656037ull) {
        for (auto c : s) h = (h ^ (uint8_t)c) * 1099511628211ull;
        return h;
    }
    static string Hex(uint64_t h, int digits) {
        char buf[17];
        snprintf(buf, sizeof buf, "%016llx", (unsigned long long)h);
        return string(buf, digits);
    }

    // `t` with each identifier `ren` knows replaced.
    static string Renamed(string_view t, const unordered_map<string, string> &ren) {
        string out;
        size_t at = 0;
        EachIdent(t, [&](size_t b, size_t e) {
            auto it = ren.find(string(t.substr(b, e - b)));
            if (it == ren.end()) return;
            out.append(t.substr(at, b - at));
            out += it->second;
            at = e;
        });
        if (!at) return string(t);
        out.append(t.substr(at));
        return out;
    }

    // Codegen numbers its string, blob and location objects by first use
    // (gs_str12), so one new literal renames every later one, and every unit
    // naming those would change. Here they are named by what they hold.
    void StableDataNames() {
        static const char *const prefixes[] = { "gs_strv", "gs_str", "gs_blob", "gs_file", "gs_loc" };
        auto numbered = [&](const string &n) -> string {
            for (auto p : prefixes) {
                auto len = strlen(p);
                if (n.size() <= len || n.compare(0, len, p)) continue;
                if (std::all_of(n.begin() + len, n.end(), [](char c) { return isdigit((unsigned char)c); }))
                    return p;
            }
            return "";
        };
        unordered_map<string, string> ren;
        unordered_set<string> taken;
        for (auto &it : items) {
            if (it.kind != DATA) continue;
            auto prefix = numbered(it.names[0]);
            if (prefix.empty()) continue;
            unordered_map<string, string> self = { { it.names[0], "@" } };
            auto body = Renamed(Renamed(it.defn, ren), self);
            auto h = Hash(body);
            auto name = cat(prefix, "_", Hex(h, 12));
            // Two objects holding the same are told apart, in order.
            for (int n = 2; !taken.insert(name).second; n++) name = cat(prefix, "_", Hex(h, 12), "_", n);
            ren[it.names[0]] = name;
        }
        Rename(ren);
    }

    // Source lines are in nearly every function's C, as the arguments of
    // its failure paths (gs_file_<h>, 120) and in the location objects it
    // names, so a line added to a source file would change every function
    // after it. Each function's lines become relative to a base of its own,
    // gs_lb_<fn>_<k> for each file, and its location objects its own, gs_lc_<fn>_<k>, which
    // all go in the unit "lines": an edit then changes the functions it
    // touches and that unit, which holds only numbers.
    void RelativeLines() {
        unordered_set<string> files;
        unordered_map<string, pair<string, int64_t>> locs;   // gs_loc_<h> -> its file, line
        for (auto &it : items) {
            if (it.kind != DATA) continue;
            auto &n = it.names[0];
            if (Starts(n, "gs_file_")) {
                files.insert(n);
            } else if (Starts(n, "gs_loc_")) {
                string_view d = it.defn;   // const gs_loc <name> = { <file>, <line> };
                auto b = d.find("= { "), c = d.find(", ", b), e = d.find(" }", c);
                if (b == string_view::npos || c == string_view::npos || e == string_view::npos) continue;
                auto line = d.substr(c + 2, e - c - 2);
                if (line.empty() || !std::all_of(line.begin(), line.end(),
                                                 [](char ch) { return isdigit((unsigned char)ch); }))
                    continue;
                locs[n] = { string(d.substr(b + 4, c - b - 4)), atoll(string(line).c_str()) };
            }
        }
        vector<Item> added;
        for (auto &it : items) {
            if (it.kind != FN && it.kind != INLINE) continue;
            string_view t = it.defn;
            // A base per source file: the function holds code inlined from
            // others, whose lines move with edits to those.
            struct Hit { size_t b, e; int64_t line; const string *loc; string file; };
            vector<Hit> hits;
            vector<pair<string, int64_t>> bases;   // file -> its lowest line, by first appearance
            auto note = [&](const string &file, int64_t line) {
                for (auto &[f, b] : bases) if (f == file) { b = std::min(b, line); return; }
                bases.push_back({ file, line });
            };
            EachIdent(t, [&](size_t b, size_t e) {
                auto id = t.substr(b, e - b);
                if (Starts(id, "gs_file_")) {
                    if (t.substr(e, 2) != ", " || !files.count(string(id))) return;
                    auto d = e + 2, f = d;
                    while (f < t.size() && isdigit((unsigned char)t[f])) f++;
                    if (f == d || (f < t.size() && (IdChar(t[f]) || t[f] == '.'))) return;
                    auto line = atoll(string(t.substr(d, f - d)).c_str());
                    hits.push_back({ d, f, line, nullptr, string(id) });
                    note(string(id), line);
                } else if (Starts(id, "gs_loc_")) {
                    auto l = locs.find(string(id));
                    if (l == locs.end()) return;
                    hits.push_back({ b, e, l->second.second, &l->first, l->second.first });
                    note(l->second.first, l->second.second);
                }
            });
            if (hits.empty()) continue;
            auto &fn = it.names[0];
            auto lbof = [&](const string &file, int64_t &base) {
                for (size_t k = 0; k < bases.size(); k++)
                    if (bases[k].first == file) { base = bases[k].second; return cat("gs_lb_", fn, "_", k); }
                return string();
            };
            string out;
            size_t at = 0;
            unordered_map<string, string> mine;
            for (auto &h : hits) {
                out.append(t.substr(at, h.b - at));
                at = h.e;
                if (!h.loc) {
                    int64_t base = 0;
                    auto lb = lbof(h.file, base);
                    Append(out, "(", lb, " + ", h.line - base, ")");
                    continue;
                }
                auto [lc, isnew] = mine.try_emplace(*h.loc, cat("gs_lc_", fn, "_", mine.size()));
                out += lc->second;
                if (!isnew) continue;
                Item d;
                d.kind = DATA;
                d.names = { lc->second };
                d.defn = cat("const gs_loc ", lc->second, " = { ", locs[*h.loc].first, ", ", h.line, " };\n");
                d.decl = cat("extern const gs_loc ", lc->second, ";\n");
                d.unit = "lines";
                added.push_back(std::move(d));
            }
            out.append(t.substr(at));
            it.defn = std::move(out);
            for (size_t k = 0; k < bases.size(); k++) {
                auto lb = cat("gs_lb_", fn, "_", k);
                Item d;
                d.kind = DATA;
                d.names = { lb };
                d.defn = cat("const int32_t ", lb, " = ", bases[k].second, ";\n");
                d.decl = cat("extern const int32_t ", lb, ";\n");
                d.unit = "lines";
                added.push_back(std::move(d));
            }
        }
        for (auto &d : added) items.push_back(std::move(d));
        // The shared location objects no function names any more would still
        // be placed, by a name that changes with their line.
        unordered_set<string> named;
        for (auto &it : items) {
            if (it.kind == DATA && Starts(it.names[0], "gs_loc_")) continue;
            for (auto s : { &it.text, &it.decl, &it.defn }) {
                string_view v = *s;
                EachIdent(v, [&](size_t b, size_t e) {
                    if (Starts(v.substr(b, e - b), "gs_loc_")) named.insert(string(v.substr(b, e - b)));
                });
            }
        }
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &it) {
            return it.kind == DATA && locs.count(it.names[0]) && !named.count(it.names[0]);
        }), items.end());
    }

    // Replaces the identifiers `ren` names in every item.
    void Rename(const unordered_map<string, string> &ren) {
        if (ren.empty()) return;
        for (auto &it : items) {
            it.text = Renamed(it.text, ren);
            it.decl = Renamed(it.decl, ren);
            it.defn = Renamed(it.defn, ren);
            for (auto &n : it.names) if (auto r = ren.find(n); r != ren.end()) n = r->second;
        }
    }

    void Link(string_view everywhere, vector<int> &everydeps, string_view hdr, vector<int> &hdrdeps) {
        // A function with a prototype and a body: the body's item is both.
        unordered_set<string> defined;
        for (auto &it : items) if (it.kind == FN || it.kind == INLINE) defined.insert(it.names[0]);
        items.erase(std::remove_if(items.begin(), items.end(), [&](const Item &it) {
            return it.kind == PROTO && defined.count(it.names[0]);
        }), items.end());
        for (size_t i = 0; i < items.size(); i++) {
            for (auto &n : items[i].names) {
                auto [at, isnew] = nameids.try_emplace(n, (int)byname.size());
                if (isnew) byname.emplace_back();
                byname[at->second].push_back((int)i);
            }
        }
        auto scan = [&](string_view t, vector<int> &out, const Item *self) {
            EachIdent(t, [&](size_t b, size_t e) {
                auto at = nameids.find(t.substr(b, e - b));
                if (at != nameids.end()) out.push_back(at->second);
            });
            std::sort(out.begin(), out.end());
            out.erase(std::unique(out.begin(), out.end()), out.end());
            (void)self;
        };
        for (auto &it : items) {
            scan(it.kind == FN || it.kind == DATA || it.kind == INLINE ? string_view(it.defn)
                                                                       : string_view(it.text),
                 it.deps, &it);
            if (!it.decl.empty()) scan(it.decl, it.decldeps, &it);
        }
        scan(everywhere, everydeps, nullptr);
        scan(hdr, hdrdeps, nullptr);
    }
};

// The units of a program: `pre` is what starts the single file (the banner,
// the defines and runtime.h), `ext` the extern support that follows the
// types, `cg` the code generator that has run. `targetkb` is about how much
// definition text a unit holds; a larger definition has a unit of its own.
// `incdir` is the directory the headers' relative names are relative to.
inline vector<SplitUnit> SplitProgram(CodeGen &cg, const string &pre, const string &ext,
                                      int64_t targetkb, const string &incdir) {
    Splitter s;
    // The includes and extern prototypes are only in the assembled text.
    string_view result = cg.result;
    auto cut = [&](string_view from, string_view to) {
        auto b = result.find(from), e = result.find(to);
        if (b == string_view::npos || e == string_view::npos || e < b)
            throw CompileError { "--split does not recognize the generated C's sections" };
        return result.substr(b + from.size(), e - b - from.size());
    };
    string includes(cut("\n/* ---- includes ---- */\n", "\n/* ---- extern prototypes ---- */\n"));
    auto externs = cut("\n/* ---- extern prototypes ---- */\n", "\n/* ---- prototypes ---- */\n");
    // The runtime keeps a thread program's stack block and globals instance
    // in static thread-locals, which every unit then has its own of. They
    // are set as a thread program starts and not after, so the code that
    // sets them hands them to every unit there. (Declared extern instead,
    // they cost clang minutes on a large function.)
    string code;
    {
        string_view c = cg.code;
        for (size_t b = 0; b < c.size();) {
            auto e = c.find('\n', b);
            e = e == string_view::npos ? c.size() : e + 1;
            auto line = c.substr(b, e - b);
            code.append(line);
            if (Splitter::Starts(line, "    gs_gl = ") || Splitter::Starts(line, "    gs_stack_block(") ||
                Splitter::Starts(line, "    gs_rt_init("))
                code += "    gs_split_sync(gs_stks, gs_nstks, gs_gl);\n";
            b = e;
        }
    }
    s.Parse(cg.tdecls, true);
    s.Parse(cg.pdata, true);
    s.Parse(cg.data, false);
    auto ext0 = s.items.size();
    s.Parse(externs, false);
    auto ext1 = s.items.size();
    s.Parse(cg.protos, false);
    s.Parse("void gs_split_sync(gs_stack *p0, int64_t p1, void *p2);\n", false);
    s.Parse(code, false);
    s.StableDataNames();
    // The --include and sidecar headers are written relative to where the
    // one file would go, `incdir`; the units go elsewhere, so they name them
    // by absolute path. What the headers (and the headers they include) use
    // of the generated C, every unit has.
    string inclines, headertext;
    {
        set<string> seen;
        std::function<void(const filesystem::path &)> read = [&](const filesystem::path &p) {
            std::error_code ec;
            auto canon = filesystem::weakly_canonical(p, ec).string();
            if (!seen.insert(canon).second) return;
            string text;
            if (!LoadFile(canon, text)) return;
            headertext += text;
            headertext += "\n";
            string_view t = text;
            for (size_t b = 0; b < t.size();) {
                auto e = t.find('\n', b);
                auto line = t.substr(b, (e == string_view::npos ? t.size() : e) - b);
                b = e == string_view::npos ? t.size() : e + 1;
                auto q = line.find("#include \"");
                if (q == string_view::npos || line.substr(0, q).find_first_not_of(" \t") != string_view::npos)
                    continue;
                auto name = line.substr(q + 10);
                name = name.substr(0, name.find('"'));
                read(p.parent_path() / filesystem::path(string(name)));
            }
        };
        string_view t = includes;
        for (size_t b = 0; b < t.size();) {
            auto e = t.find('\n', b);
            auto line = t.substr(b, (e == string_view::npos ? t.size() : e) - b);
            b = e == string_view::npos ? t.size() : e + 1;
            if (!Splitter::Starts(line, "#include \"")) { inclines += cat(line, "\n"); continue; }
            auto name = line.substr(10);
            name = name.substr(0, name.find('"'));
            filesystem::path p { string(name) };
            if (p.is_relative() && !incdir.empty()) p = filesystem::path(incdir) / p;
            if (!filesystem::exists(p)) { inclines += cat(line, "\n"); continue; }   // On the include path.
            p = filesystem::absolute(p).lexically_normal();
            auto abs = p.generic_string();
            inclines += cat("#include \"", abs, "\"\n");
            read(p);
        }
    }
    includes = inclines;
    // The headers are in one unit only, `headers`: they may keep state at
    // file scope (a JIT's code table), which each unit holding them would
    // have its own of. An extern fn a header implements is static there, so
    // that unit forwards to it from gs_hx_<name>, which the other units call.
    string forwarders;
    {
        unordered_set<string> called;
        string_view h = headertext;
        Splitter::EachIdent(h, [&](size_t b, size_t e) {
            while (e < h.size() && (h[e] == ' ' || h[e] == '\t')) e++;
            if (e < h.size() && h[e] == '(') called.insert(string(h.substr(b, e - b)));
        });
        unordered_map<string, string> ren;
        for (auto i = ext0; i < ext1; i++) {
            auto &it = s.items[i];
            if (it.kind != Splitter::PROTO || !called.count(it.names[0])) continue;
            auto &n = it.names[0];
            // Packages that each declare one extern declare it once each.
            if (ren.count(n)) continue;
            string_view t = Splitter::RTrim(it.text);
            auto at = t.find(cat(n, "("));
            auto close = t.rfind(')');
            if (at == string_view::npos || close == string_view::npos || close < at) continue;
            auto ret = Splitter::RTrim(t.substr(0, at));
            auto args = t.substr(at + n.size() + 1, close - at - n.size() - 1);
            string params, pass;
            if (args.find_first_not_of(" \t") == string_view::npos || args == "void") {
                params = "void";
            } else {
                int depth = 0, k = 0;
                size_t b = 0;
                for (size_t c = 0; c <= args.size(); c++) {
                    if (c < args.size() && (args[c] == '(' || args[c] == '[')) depth++;
                    else if (c < args.size() && (args[c] == ')' || args[c] == ']')) depth--;
                    else if (c == args.size() || (args[c] == ',' && !depth)) {
                        Append(params, k ? ", " : "", args.substr(b, c - b), " p", k);
                        Append(pass, k ? ", " : "", "p", k);
                        k++;
                        b = c + 1;
                    }
                }
            }
            auto hx = cat("gs_hx_", n);
            Append(forwarders, ret, " ", hx, "(", params, ") { ", ret == "void" ? "" : "return ",
                   n, "(", pass, "); }\n");
            ren[n] = hx;
        }
        s.Rename(ren);
    }
    // After the forwarders, which find the externs by their place in items.
    s.RelativeLines();
    vector<int> everydeps, hdrdeps;
    s.Link(ext, everydeps, cat(includes, headertext, forwarders), hdrdeps);
    auto hasheaders = !includes.empty();

    auto &items = s.items;
    auto owned = [&](const Splitter::Item &it) { return it.kind == Splitter::FN || it.kind == Splitter::DATA; };
    size_t total = 0;
    for (auto &it : items) if (owned(it)) total += it.defn.size();
    auto target = (size_t)std::max<int64_t>(targetkb, 1) * 1000;
    size_t buckets = 1;
    while (buckets * target < total) buckets *= 2;
    map<string, vector<int>> units;
    for (size_t i = 0; i < items.size(); i++) {
        auto &it = items[i];
        if (!owned(it)) continue;
        string unit;
        if (!it.unit.empty()) {
            unit = it.unit;
        } else if (it.names[0] == "main") {
            unit = "main";
        } else if (it.defn.size() > target) {
            unit = cat("big_", it.names[0]);
        } else {
            // A specialization's number is not part of where it lives: it
            // shifts when an unrelated one is added.
            string_view base = it.names[0];
            auto e = base.size();
            while (e && isdigit((unsigned char)base[e - 1])) e--;
            if (e && e < base.size() && base[e - 1] == '_') base = base.substr(0, e - 1);
            char buf[16];
            snprintf(buf, sizeof buf, "u%04d", (int)(Splitter::Hash(base) % buckets));
            unit = buf;
        }
        units[unit].push_back((int)i);
    }
    if (!units.count("main")) units["main"];
    if (hasheaders) units["headers"];

    vector<SplitUnit> out;
    vector<uint8_t> need(items.size());   // 1: its declaration, 2: all of it.
    vector<pair<int, bool>> work;
    vector<int> chosen;
    for (auto &[name, mine] : units) {
        std::fill(need.begin(), need.end(), 0);
        chosen.clear();
        auto take = [&](int i, bool full) {
            // Only a definition's own unit holds more than its declaration.
            uint8_t level = full || !owned(items[i]) ? 2 : 1;
            if (need[i] >= level) return;
            if (!need[i]) chosen.push_back(i);
            need[i] = level;
            work.push_back({ i, level == 2 });
        };
        auto want = [&](int nameid) { for (auto i : s.byname[nameid]) take(i, false); };
        for (auto i : mine) take(i, true);
        auto isheaders = hasheaders && name == "headers";
        for (auto n : everydeps) want(n);
        if (isheaders) for (auto n : hdrdeps) want(n);
        while (!work.empty()) {
            auto [i, full] = work.back();
            work.pop_back();
            for (auto n : full ? items[i].deps : items[i].decldeps) want(n);
        }
        std::sort(chosen.begin(), chosen.end());
        SplitUnit u;
        u.name = name;
        auto &t = u.text;
        t = pre;
        auto packed = false;
        for (auto i : chosen) {
            auto &it = items[i];
            if (it.kind != Splitter::TYPE) continue;
            if (it.packed != packed) t += it.packed ? "#pragma pack(push, 1)\n" : "#pragma pack(pop)\n";
            packed = it.packed;
            t += it.text;
        }
        if (packed) t += "#pragma pack(pop)\n";
        // Every declaration first, so that definitions may come in any order.
        for (auto i : chosen)
            if (!items[i].decl.empty() && !(items[i].anontype && need[i] == 2)) t += items[i].decl;
        for (auto i : chosen)
            if (items[i].kind == Splitter::MACRO || items[i].kind == Splitter::PROTO) t += items[i].text;
        t += ext;
        if (isheaders) {
            // The object is kept by a hash of this text, which has to change
            // when a header does.
            Append(t, "/* headers ", Splitter::Hex(Splitter::Hash(headertext), 16), " */\n", includes,
                   forwarders);
        }
        for (auto i : chosen) if (items[i].kind == Splitter::DATA && need[i] == 2) t += items[i].defn;
        for (auto i : chosen) if (items[i].kind == Splitter::INLINE) t += items[i].defn;
        for (auto i : chosen) if (items[i].kind == Splitter::FN && need[i] == 2) t += items[i].defn;
        Append(t, "void gs_split_set_", name, "(gs_stack *s, int64_t n, void *gl) "
                  "{ gs_stks = s; gs_nstks = n; gs_gl = gl; }\n");
        if (name == "main") {
            for (auto &[other, _] : units)
                if (other != "main")
                    Append(t, "void gs_split_set_", other, "(gs_stack *s, int64_t n, void *gl);\n");
            t += "void gs_split_sync(gs_stack *s, int64_t n, void *gl) {\n";
            for (auto &[other, _] : units) Append(t, "    gs_split_set_", other, "(s, n, gl);\n");
            t += "}\n";
        }
        out.push_back(std::move(u));
    }
    return out;
}

}  // namespace goose
