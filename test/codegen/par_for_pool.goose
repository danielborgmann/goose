import std;
// par_threads, par_spin_us and par_warm: a loop gives the same result on any
// pool size, the pool is remade at the next fork after a change, each call
// returns what it set, and a warmed pool (stacks made as its threads start)
// runs a body that needs data stacks.
fn total(n: i64) -> i64 {
    var out: i64[>..] = [];
    out.resize(n, 0);
    par for i in n { out[i] = i * i % 7; }
    var s = 0;
    for v in out { s += v; }
    return s;
}

// A body that needs data stacks: a growable local per iteration.
fn spread(n: i64) -> i64 {
    var out: i64[>..] = [];
    out.resize(n, 0);
    par for i in n {
        var xs: i64[>..] = [];
        for k in i % 5 + 1 { xs.push(k * i); }
        var s = 0;
        for v in xs { s += v; }
        out[i] = s;
    }
    var s = 0;
    for v in out { s += v; }
    return s;
}

fn main() {
    let warmed = par_warm();
    print(warmed >= 1, " ", spread(500));
    let first = total(1000);
    var same = true;
    for k in [1, 2, 3, 8, 0] {
        let got = par_threads(k);
        same = same && got >= 1 && (k == 0 || got <= k);
        same = same && total(1000) == first;
    }
    print(first, " ", same);
    print(par_spin_us(3000), " ", par_spin_us(0), " ", par_spin_us(-1));
    print(total(1000) == first);
    print(par_threads(3) <= 3, " ", par_warm() <= 3, " ", spread(500));
}
