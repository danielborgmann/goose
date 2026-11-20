// The cost of a `par for` fork and join on the pool (FastGoose's
// tools/par_for.md), next to spawn_cost.goose's share-nothing round: a fork
// over a few elements per thread that do almost nothing, so what it measures
// is the fork; then a loop with work in it, which shows what the pool gains.
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
    var s = 0;
    for v in out { s ^= v; }
    print("checksum ", s, ", hardware_threads ", hardware_threads());
}
