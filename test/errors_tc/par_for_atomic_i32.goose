// error: .atomic_or works on i64 and u64 words
import std;

fn main() {
    var marks: i32[>..] = [];
    marks.resize(4, 0);
    print(marks.atomic_or(0, 1));
}
