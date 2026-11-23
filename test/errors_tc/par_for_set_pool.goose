// error: a par for body cannot do this: it calls par_threads
// R3: the pool's size is set between forks, not in one.
import std;

fn main() {
    var out: i64[>..] = [];
    out.resize(4, 0);
    par for i in 4 { out[i] = par_threads(2); }
    print(out);
}
