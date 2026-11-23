// error: globals and the variables of the function around the loop are read-only
// R3 in the checker: a reference to a global returned by a function the body calls.
var g_a: i64[>..] = [];
fn get() -> i64[>..]& { return g_a; }
fn main() {
    var out: i64[>..] = [];
    out.resize(8, 0);
    par for i in 0..8 { get().push(i); out[i] = i; }
    print(g_a.len);
}
