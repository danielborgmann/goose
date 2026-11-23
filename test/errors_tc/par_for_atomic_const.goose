// error: atomic_or
// A const view cannot be written, atomically or not.
import std;

fn mark(m: const u64[:]) -> u64 { return m.atomic_or(0, 1); }

fn main() {
    var marks: u64[>..] = [];
    marks.resize(4, 0);
    print(mark(marks));
}
