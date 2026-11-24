// error: calls the extern fn c_abs (an extern pure fn may be called)
// R3: an extern not declared pure.
import std;
extern fn c_abs(x: i32) -> i32 = "abs";

fn main() {
    var out: i64[>..] = [];
    for i in 10 { out.push(0); }
    par for i in out.len { out[i] = c_abs(i as i32) as i64; }
}
