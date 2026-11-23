// error: globals and the variables of the function around the loop are read-only
// A global struct holding a writable reference, passed by value: what it holds is read-only too.
struct H { a: i64[>..]& }
var g_a: i64[>..] = [];
var g_h = H { a: g_a };
fn put(h: H, v: i64) { h.a.push(v); }
fn main() {
    var out: i64[>..] = [];
    out.resize(8, 0);
    par for i in 0..8 { put(g_h, i); out[i] = i; }
    print(g_a.len);
}
