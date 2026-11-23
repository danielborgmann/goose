import std;
// par_threads and par_spin_us: a loop gives the same result on any pool size,
// the pool is remade at the next fork after a change, and each call returns
// what it set.
fn total(n: i64) -> i64 {
    var out: i64[>..] = [];
    out.resize(n, 0);
    par for i in n { out[i] = i * i % 7; }
    var s = 0;
    for v in out { s += v; }
    return s;
}

fn main() {
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
}
