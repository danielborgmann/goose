// Only main makes racy views (tools/racy_views.md): a worker's arrays end
// with the worker, so a view of them could outlive what it reads.
thread_fn w() {
    var a: i64[>..] = [1, 2];
    let v = racy(a);
    print(v.len);
}

fn main() {
    print("start");
    thread_wait(thread_spawn(w));
}
