import std;
// An extern pure fn (FastGoose): its declarer vouches it is safe from any
// par for iteration, so a body may call it.
extern pure fn c_abs(x: i32) -> i32 = "abs";

fn main() {
    var out: i64[>..] = [];
    for i in 64 { out.push(0); }
    par for i in out.len { out[i] = c_abs((i * 3 - 90) as i32) as i64; }
    var s = 0;
    for v in out { s += v; }
    print(s);
}
