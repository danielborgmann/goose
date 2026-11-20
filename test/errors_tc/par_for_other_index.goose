// error: cannot write out, a variable of the function around the loop
// R2: one use of out not by the loop's index makes it captured.
import std;

var counter = 0;
fn bump() { counter += 1; }
fn shout(x: i64) { print(x); }
fn make() -> i64[>..] { var r: i64[>..] = []; r.push(1); return r; }

fn main() {
    var out: i64[>..] = [];
    for i in 10 { out.push(0); }
    var total = 0;
    let n = 4;
    par for i in 10 { out[i] = 1; out[0] = 2; }
    print(out, total);
}
