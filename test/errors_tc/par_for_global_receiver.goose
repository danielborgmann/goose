// error: globals and the variables of the function around the loop are read-only
// R3: a global passed as the receiver of a call, to a writable parameter.
import std;

var grid: i64[8];
fn clear_at(g: i64[8]&, i: i64) { g[i] = 0; }

fn main() {
    par for i in 8 { grid.clear_at(i); }
    print(grid[0]);
}
