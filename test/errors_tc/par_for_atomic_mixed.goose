// error: a par for body cannot write marks, a variable of the function around the loop
// A shared array is one the body reaches only atomically: a plain read of it
// would race with another iteration's atomic_or.
import std;

fn main() {
    var marks: u64[>..] = [];
    marks.resize(4, 0);
    var out: u64[>..] = [];
    out.resize(64, 0);
    par for i in 64 {
        let old = marks.atomic_or(i / 16, 1);
        out[i] = old + marks[0];
    }
    print(out[0]);
}
