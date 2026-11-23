// error: storing a read-only reference of type const i64[>..]& in a slot of type i64[>..]&
// R3 in the checker: a reference to a global, taken in the body into a field, is read-only.
struct Sc { bail: bool = false }
struct SxPar { a: i64[>..]&, sc: Sc& }
var g_a: i64[>..] = [];
fn touch(scx: SxPar&, v: i64) -> i64 { scx.a.push(v); return scx.a.len; }
fn main() {
    var out: i64[>..] = [];
    out.resize(8, 0);
    par for i in 0..8 {
        var lsc = Sc {};
        var px = SxPar { a: g_a, sc: lsc };
        out[i] = touch(px, i);
    }
    print(g_a.len);
}
