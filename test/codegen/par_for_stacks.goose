// par for bodies that need data stacks (tools/par_for.md, step 5): a tree
// walk per iteration with a growable work list, and a callee that builds a
// growable result. On a pool thread the body runs on the thread's own stack
// block, rewound after each chunk.
import std;

// The values of the subtree under `root` (children of i: 3i+1 .. 3i+3).
fn subtree(vals: const i64[:], root: i64) -> i64 {
    var todo: i64[>..] = [];
    todo.push(root);
    var sum = 0;
    while todo.len > 0 {
        let i = todo.pop();
        sum += vals[i];
        for c in 3 * i + 1..min(3 * i + 4, vals.len) { todo.push(c); }
    }
    return sum;
}

// The digits of v, most significant first.
fn digits(v: i64) -> i64[>..] {
    var ds: i64[>..] = [];
    var x = v;
    while x > 0 {
        ds.push(x % 10);
        x /= 10;
    }
    ds.reverse();
    return ds;
}

fn main() {
    let n = 30000;
    var vals: i64[>..] = [];
    for i in n { vals.push(i % 13); }
    var sums: i64[>..] = [];
    for i in 2000 { sums.push(0); }
    for round in 20 {
        par for r in sums.len {
            // A walk in the body itself, and one through a call.
            var todo: i64[>..] = [];
            todo.push(r);
            var count = 0;
            while todo.len > 0 {
                let i = todo.pop();
                count++;
                for c in 3 * i + 1..min(3 * i + 4, n) { todo.push(c); }
            }
            sums[r] = subtree(vals, r) * 100000 + count;
        }
    }
    var s = 0;
    for v in sums { s = (s * 31 + v) % 1000000007; }
    print(s);
    print(sums[0], " ", sums[1], " ", sums[1999]);
    var widths: i64[>..] = [];
    for i in 1000 { widths.push(0); }
    par for i in widths.len { widths[i] = digits(i * i).len; }
    var w = 0;
    for v in widths { w += v; }
    print(w);
}
