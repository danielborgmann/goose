// error: racy() takes a resizable array
// Racy views (tools/racy_views.md): only main's own arrays, whose base never moves.
struct S { n: i64, xs: i64[>..]& }

fn f(xs: i64[:]) { let v = racy(xs); }

fn main() {}
