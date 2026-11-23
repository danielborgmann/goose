// error: a racy view is read-only
// Racy views (tools/racy_views.md): nor a field of main's struct.
struct S { n: i64, xs: i64[>..]& }

thread_fn w(v: racy S) { v.n = 1; }

fn main() {}
