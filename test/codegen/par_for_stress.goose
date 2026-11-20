// par for (tools/par_for.md) under load: many forks over a large array and many small ones, a fork inside a fork's body, and a body that needs a data stack.
import std;

fn main() {
    let n = 100000;
    var out: i64[>..] = [];
    for i in n { out.push(0); }
    for round in 50 {
        par for i in n { out[i] = out[i] + i % 7; }
    }
    var s = 0;
    for v in out { s += v; }
    print(s);
    // Many small forks.
    var small: i64[>..] = [];
    for i in 64 { small.push(0); }
    for round in 2000 {
        par for i in 64 { small[i] = small[i] + 1; }
    }
    s = 0;
    for v in small { s += v; }
    print(s);
    // A fork inside a fork's body runs where it is.
    let parts = 8;
    par for k in parts {
        let c = out.chunk(k, parts);
        par for &v in c { v = k; }
    }
    s = 0;
    for v in out { s += v; }
    print(s);
    // A growable local: the body needs a data stack.
    par for i in 1000 {
        var t: i64[>..] = [];
        for j in i % 5 { t.push(j); }
        out[i] = t.len;
    }
    s = 0;
    for i in 1000 { s += out[i]; }
    print(s);
}
