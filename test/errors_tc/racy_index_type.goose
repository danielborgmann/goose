// error: is an index type, which indexes without a check
// Racy views (tools/racy_views.md): an index read racily would skip its bounds check.
struct S { n: i64, xs: i64[>..]& }

var a: i64[>..] = [1];
fn f() { var ix: (u32 in a)[>..] = []; let v = racy(ix); }

fn main() {}
