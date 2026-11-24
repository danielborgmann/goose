// error: a par for body cannot call back into the recursive cycle it is in
// A par for in a recursive cycle whose body calls back into it.
import std;
struct Ctx { d: const i64[>..<]&, items: i64[>..<]&, alt: i64[>..<]& }
recursive fn lay(L: Ctx&, depth: i64) {
    L.items.push(L.d[0] + depth);
    if depth == 0 {
        var sums: i64[8];
        par for k in 8 {
            var items: i64[>..<] = [];
            var alt: i64[>..<] = [];
            var T = Ctx { d: L.d, items: items, alt: alt };
            lay(T, k + 1);
            sums[k] = items.len;
        }
        print(sums);
    }
    if depth < 3 { lay(L, depth + 1); }
}
fn main() {
    var d: i64[>..<] = [];
    d.push(10);
    var items: i64[>..<] = [];
    var alt: i64[>..<] = [];
    var L = Ctx { d: d, items: items, alt: alt };
    lay(L, 0);
}
