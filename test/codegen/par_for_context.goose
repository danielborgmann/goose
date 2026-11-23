import std;
// A context struct for the iterations: shared tables through const fields, the body's own
// locals through writable ones, which a function the body (and main) calls writes through.
struct Sc { bail: bool = false, n: i64 = 0 }
struct P { tab: const i64[>..]&, sc: Sc&, buf: i64[>..]& }
var g_tab: i64[>..] = [];
struct GH { a: i64[>..]& }
var g_store: i64[>..] = [];
var g_h = GH { a: g_store };
fn touch(p: P&, i: i64) -> i64 {
    p.sc.n += p.tab[i % p.tab.len];
    if (p.sc.n > 3) { p.sc.bail = true; }
    p.buf.push(i);
    return p.sc.n + p.buf.len + g_h.a.len;
}
fn fill(b: i64[>..]&, n: i64) { for k in n { b.push(k); } }
fn main() {
    for i in 5 { g_tab.push(i); }
    var out: i64[>..] = [];
    out.resize(8, 0);
    var flags: bool[>..] = [];
    flags.resize(8, false);
    par for i in 0..8 {
        var lsc = Sc {};
        var lb: i64[>..] = [];
        fill(lb, i);
        var px = P { tab: g_tab, sc: lsc, buf: lb };
        out[i] = touch(px, i);
        flags[i] = lsc.bail;
    }
    print(out, flags);
    var lsc = Sc {};
    var lb: i64[>..] = [];
    var px = P { tab: g_tab, sc: lsc, buf: lb };
    print(touch(px, 2));
    g_h.a.push(1);
    print(g_store.len);
}
