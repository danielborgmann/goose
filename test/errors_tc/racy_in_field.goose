// error: a racy view lives in a local, a parameter or a result, not in a field
// Racy views (tools/racy_views.md): a view does not outlive the code that holds it.
struct S { n: i64, xs: i64[>..]& }

struct H { v: racy i64[:] }

fn main() {}
