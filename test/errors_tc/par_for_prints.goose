// error: a par for body cannot do this: it prints
// R3: no output from a body.
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
    par for i in 10 { print(i); }
    print(out, total);
}
