// error: cannot write out, a variable of the function around the loop
// R2: a chunk count must be a name or literal the body does not change.
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
    par for k in n { for &v in out.chunk(k, k + 1) { v = 1; } }
    print(out, total);
}
