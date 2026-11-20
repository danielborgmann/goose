// error: cannot write out, a variable of the function around the loop
// R2: out[i - 1] is not this iteration's element.
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
    par for i in 1..10 { out[i - 1] = i; }
    print(out, total);
}
