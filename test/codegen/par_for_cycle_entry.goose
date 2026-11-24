import std;
// Each iteration of a par for outside a recursive cycle enters it afresh, its
// growable locals the outputs and a shared table read through a const field.
struct Ctx { d: const i64[>..<]&, items: i64[>..<]&, alt: i64[>..<]& }
recursive fn lay(L: Ctx&, depth: i64) {
    L.items.push(L.d[0] + depth);
    let mark = L.alt.len;
    L.alt.push(depth);
    if depth < 3 { lay(L, depth + 1); }
    L.alt.resize(mark);
}
fn main() {
    var d: i64[>..<] = [];
    d.push(10);
    var sums: i64[>..] = [];
    for k in 8 { sums.push(0); }
    par for k in sums.len {
        var items: i64[>..<] = [];
        var alt: i64[>..<] = [];
        var T = Ctx { d: d, items: items, alt: alt };
        lay(T, k);
        var s = 0;
        for v in items { s += v; }
        sums[k] = s;
    }
    print(sums);
}
