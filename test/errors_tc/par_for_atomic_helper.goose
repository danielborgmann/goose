// error: In a par for and the functions it calls, globals and the variables of the function around the loop are read-only
// Only the body's own atomic_or calls share the array: passed to a function,
// it is the enclosing function's again, read-only.
import std;

fn mark(m: u64[>..]&, w: i64) { m.atomic_or(w, 1); }

fn main() {
    var marks: u64[>..] = [];
    marks.resize(4, 0);
    par for i in 4 { mark(marks, i); }
    print(marks[0]);
}
