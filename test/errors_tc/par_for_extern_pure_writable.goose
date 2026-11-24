// error: calls the extern pure fn fill, which takes a writable reference or slice of other than numbers
// R3: an extern pure fn may write through its parameters only numbers.
import std;
struct P { a: i64, b: i64 }
extern pure fn fill(p: P&) = "gs_test_fill";

fn main() {
    var ps: P[>..] = [];
    for i in 10 { ps.push(P { a: 0, b: 0 }); }
    par for i in ps.len { fill(ps[i]); }
}
