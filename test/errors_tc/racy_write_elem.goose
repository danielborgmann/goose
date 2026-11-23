// error: a racy view is read-only
// Racy views (tools/racy_views.md): a worker cannot write main's array through a view.
struct S { n: i64, xs: i64[>..]& }

thread_fn w(v: racy i64[:]) { v[0] = 1; }

fn main() {}
