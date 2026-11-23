// error: globals and the variables of the function around the loop are read-only
// R1 the same way: a struct of the enclosing function holding a writable reference.
struct H { a: i64[>..]& }
fn put(h: H, v: i64) { h.a.push(v); }
fn main() {
    var arr: i64[>..] = [];
    var h = H { a: arr };
    var out: i64[>..] = [];
    out.resize(8, 0);
    par for i in 0..8 { put(h, i); out[i] = i; }
    print(arr.len);
}
