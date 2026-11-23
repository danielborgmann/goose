// error: a racy view is read by index, not iterated
// Racy views (tools/racy_views.md): each element is read once, by index.
struct S { n: i64, xs: i64[>..]& }

thread_fn w(v: racy i64[:]) { for x in v { print(x); } }

fn main() {}
