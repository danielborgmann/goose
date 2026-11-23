// error: a racy view is read-only
// Racy views (tools/racy_views.md): an element is a copy, never a reference into main's memory.
struct S { n: i64, xs: i64[>..]& }

thread_fn w(v: racy i64[:]) { let r = &v[0]; print(r); }

fn main() {}
