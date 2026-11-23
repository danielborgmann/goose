// error: storing a read-only reference of type const i64[>..]& in a slot of type i64[>..]&
// R3 in the checker: a function the body calls takes a reference to a global into a field.
struct H { a: i64[>..]& }
var g_a: i64[>..] = [];
fn f(v: i64) -> i64 { var h = H { a: g_a }; h.a.push(v); return v; }
fn main() {
    var out: i64[>..] = [];
    out.resize(8, 0);
    par for i in 0..8 { out[i] = f(i); }
    print(g_a.len);
}
