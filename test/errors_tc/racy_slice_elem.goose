// error: racy views hold plain data, and i64[:] is a slice
// Racy views (tools/racy_views.md): a view's elements cannot be addresses.
struct S { n: i64, xs: i64[>..]& }

struct P { s: i64[:] }
thread_fn w(v: racy P[:]) { }

fn main() {}
