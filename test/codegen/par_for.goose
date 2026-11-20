// par for (tools/par_for.md): each form, run sequentially or forked, prints the loop's sequential results.
import std;
import math;

fn main() {
    var xs: i64[>..] = [];
    for i in 10 { xs.push(i); }
    let scale = 3;
    var out: i64[>..] = [];
    for i in 10 { out.push(0); }
    par for i in 10 { out[i] = xs[i] * scale; }
    print(out);
    par for i in 2..5 { out[i] = 0 - i; }
    print(out);
    par for &x in xs { x = x * 2; }
    print(xs);
    par for &x, i in xs { x = x + i; }
    print(xs);
    var sums: i64[>..] = [];
    for k in 4 { sums.push(0); }
    let parts = 4;
    par for k in parts {
        var t = 0;
        for v in xs.chunk(k, parts) { t += v; }
        sums[k] = t;
    }
    print(sums);
    par for k in parts { for &v in out.chunk(k, parts) { v = k; } }
    print(out);
    let n = 5;
    par for i in n { out[i] = 100 + i; }
    print(out);
    var seen: i64[>..] = [];
    for k in 10 { seen.push(0); }
    par for v, i in xs { seen[i] = v + 1; }
    print(seen);
    // continue ends one iteration; captured scalars are read.
    let base = 7;
    par for i in 10 {
        if i % 3 == 0 { continue; }
        seen[i] = base * i;
    }
    print(seen);
    // Pure callees, math, and a function value inside the body.
    var roots: f64[>..] = [];
    for i in 5 { roots.push(0.0); }
    par for i in 5 {
        roots[i] = sqrt(apply(i) { square(it) + 1 } as f64);
    }
    print(roots);
    print(scaled(xs, 2));
}

fn apply<F: fn(i64) -> i64>(v: i64) -> i64 { return F(v); }
fn square(v: i64) -> i64 { return v * v; }

// A par for in a generic function, over an untyped array.
fn scaled(a, k: i64) {
    var r: i64[>..] = [];
    for x in a { r.push(0); }
    par for &y, i in r { y = a[i] * k; }
    return r;
}
