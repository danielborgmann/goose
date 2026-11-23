// error: cannot be read: S& is a reference
// Racy views (tools/racy_views.md): a struct view reads plain fields and arrays, not other references.
struct S { n: i64, xs: i64[>..]& }

struct T { s: S&, n: i64 }
thread_fn w(v: racy T) { let x = v.s; }

fn main() {}
