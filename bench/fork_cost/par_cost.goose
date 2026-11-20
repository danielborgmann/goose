// The cost of a `par for` fork and join on the pool (FastGoose's
// tools/par_for.md), next to spawn_cost.goose's share-nothing round: a fork
// over a few elements per thread that do almost nothing, so what it measures
// is the fork; then loops with work in them, which show what the pool gains,
// the last a tree walk per iteration with a growable local.
// Run it again with GOOSE_THREADS=1 for the sequential times.
//
// Build and run (from this directory):
//   goose build par_cost.goose && ./par_cost
import std;
import os;

fn per(note: u8[>..]&, what: string, t: f64, reps: i64) {
    format(note, what);
    format_flt(note, t * 1.0e6 / reps, 2);
    format(note, " us");
}

fn main() {
    var out: i64[>..] = [];
    for i in 1 << 20 { out.push(0); }
    for m in [2, 32, 256] {
        let reps = 20000;
        par for i in m { out[i] = i; }               // warm up: makes the pool
        let t0 = clock();
        for r in reps {
            par for i in m { out[i] = out[i] + r; }
        }
        let t1 = clock();
        var note: u8[>..] = [];
        format(note, m, " iterations: ");
        per(note, "", t1 - t0, reps);
        format(note, " per fork and join");
        print(note);
    }
    // Work: each element a few hundred operations.
    for m in [1 << 10, 1 << 14, 1 << 20] {
        let reps = (1 << 22) / m;
        let t0 = clock();
        for r in reps {
            par for i in m {
                var x = out[i] + r;
                for k in 100 { x = (x * 1103515245 + 12345) % 2147483648; }
                out[i] = x;
            }
        }
        let t1 = clock();
        var note: u8[>..] = [];
        format(note, m, " iterations of 100 steps: ");
        per(note, "", t1 - t0, reps);
        format(note, " per loop");
        print(note);
    }
    // A tree walk per iteration with a growable work list, which runs on each
    // pool thread's own data stacks (children of i: 3i+1 .. 3i+3).
    let nodes = 1 << 16;
    var sums: i64[>..] = [];
    for i in 4096 { sums.push(0); }
    let reps = 5;
    let t0 = clock();
    for r in reps {
        par for k in sums.len {
            var todo: i64[>..] = [];
            todo.push(k % 64);
            var sum = 0;
            while todo.len > 0 {
                let i = todo.pop();
                sum += out[i] % 7;
                for c in 3 * i + 1..min(3 * i + 4, nodes) { todo.push(c); }
            }
            sums[k] = sum;
        }
    }
    let t1 = clock();
    var note: u8[>..] = [];
    format(note, sums.len, " tree walks with a growable work list: ");
    per(note, "", t1 - t0, reps);
    format(note, " per loop");
    print(note);
    var s = 0;
    for v in out { s ^= v; }
    print("checksum ", s, ", hardware_threads ", hardware_threads());
}
