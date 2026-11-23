import std;
// atomic_or and atomic_load: a par for marks a graph's nodes in rounds, the
// mark bits a u64 bitset its iterations share; each round's new frontier goes
// into the parts' own runs of next (the counts pass, then the marking pass).
// The marks must match a plain sequential walk's, on any number of threads.

// Nodes 5 mod 11 have no edge into them, so the walk leaves them unmarked.
fn edge(i: i64, k: i64, n: i64) -> i64 {
    let j = (i * 7 + k * 13 + i / 3) % n;
    if (j % 11 == 5) { return i; }
    return j;
}

fn main() {
    let n = 5000;
    let deg = 3;
    var seq: bool[>..] = [];
    seq.resize(n, false);
    var stack: i64[>..] = [];
    stack.push(0);
    seq[0] = true;
    while stack.len > 0 {
        let i = stack.pop();
        for k in deg {
            let j = edge(i, k, n);
            if (!seq[j]) {
                seq[j] = true;
                stack.push(j);
            }
        }
    }

    var marks: u64[>..] = [];
    marks.resize((n + 63) / 64, 0);
    let first = marks.atomic_or(0, 1);
    var total: i64[>..] = [];    // Nodes marked by the rounds, counted atomically.
    total.resize(1, 1);
    var frontier: i64[>..] = [];
    frontier.push(0);
    var rounds = 0;
    let parts = 16;
    while frontier.len > 0 {
        rounds++;
        // Children per part, a bound for its run of next.
        var counts: i64[>..] = [];
        counts.resize(parts, 0);
        par for k in parts {
            var c = 0;
            for f in frontier.chunk(k, parts) { c += deg; }
            counts[k] = c;
        }
        var bounds: i64[>..] = [];
        bounds.push(0);
        for k in parts { bounds.push(bounds[k] + counts[k]); }
        var next: i64[>..] = [];
        next.resize(bounds[parts], -1);
        var lens: i64[>..] = [];
        lens.resize(parts, 0);
        par for k in parts {
            var m = 0;
            var out = next.chunk_at(bounds, k);
            for f in frontier.chunk(k, parts) {
                for e in deg {
                    let j = edge(f, e, n);
                    let bit = (1 as u64) << (j % 64);
                    if ((marks.atomic_or(j / 64, bit) & bit) == 0) {
                        out[m] = j;
                        m++;
                        total.atomic_add(0, 1);
                    }
                }
            }
            lens[k] = m;
        }
        frontier.clear();
        for k in parts {
            for t in lens[k] { frontier.push(next[bounds[k] + t]); }
        }
    }

    var same = first == 0;
    var seen = 0;
    for i in n {
        let set = (marks.atomic_load(i / 64) >> (i % 64) & 1) == 1;
        same = same && set == seq[i];
        if (set) { seen++; }
    }
    print(seen, " ", same, " ", rounds > 1, " ", total.atomic_load(0) == seen);
}
