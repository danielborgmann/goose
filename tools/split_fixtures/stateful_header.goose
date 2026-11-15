// A split build keeps a header's file-scope state in one unit: the bumps
// below come from functions in different units, and the total must be the
// sum of all of them (tools/check_split.py, with tiny units).
import std;

extern fn sh_bump(by: i64);
extern fn sh_get() -> i64;

recursive fn bump_a(n: i64) { if n > 0 { sh_bump(1); bump_a(n - 1); } }
recursive fn bump_b(n: i64) { if n > 0 { sh_bump(10); bump_b(n - 1); } }
recursive fn bump_c(n: i64) { if n > 0 { sh_bump(100); bump_c(n - 1); } }
recursive fn bump_d(n: i64) { if n > 0 { sh_bump(1000); bump_d(n - 1); } }

fn main() {
    bump_a(3); bump_b(3); bump_c(3); bump_d(3);
    print(sh_get());
}
