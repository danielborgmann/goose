// error: a par for body cannot do this: it calls par_warm
// R3: the pool is started between forks, not in one.
import std;

fn main() {
    var out: i64[>..] = [];
    out.resize(4, 0);
    par for i in 4 { out[i] = par_warm(); }
    print(out);
}
