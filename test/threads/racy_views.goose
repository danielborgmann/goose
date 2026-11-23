// Racy views (FastGoose, tools/racy_views.md): a worker reads main's arrays
// in place through racy(), first while main waits, so what it reads is
// exact, then while main keeps growing and shrinking them, where every read
// is only bounded. One helper serves main's Vm& and the worker's racy Vm.
import os;

enum Op { Push { v: i64 }, Add, Jump { to: i32, cond: bool } }
struct Shape { id: i32, dict: bool, n: u16 }
struct Vm {
    code: Op[>..]&,
    shapes: Shape[>..<]&,
    vals: i64[>..<]&,
    epoch: i64 = 0,
    open: bool = false,
    tag: u8[4],
}
struct Seen { dicts: i64, pushed: i64, jumps: i64, vals: i64, epoch: i64, tag: i64 }

// Generic over the source: a Vm& on main, a racy Vm on the worker.
fn count_dicts(vm) -> i64 {
    var n = 0;
    for i in vm.shapes.len {
        if vm.shapes[i].dict { n++; }
    }
    return n;
}

fn survey(vm) -> Seen {
    var s = Seen { dicts: count_dicts(vm), pushed: 0, jumps: 0, vals: 0, epoch: vm.epoch,
                   tag: 0 };
    let code = vm.code;
    for i in code.len {
        let op = code[i];
        s.pushed += match op { Push p => p.v, Add => 0, Jump => 0 };
        s.jumps += match op { Jump j => if j.cond { j.to as i64 } else { 0 }, Push => 0, Add => 0 };
    }
    for i in vm.vals.len { s.vals += vm.vals[i]; }
    let t = vm.tag;
    s.tag = t[0] + t[3];
    return s;
}

thread_fn worker(vm: racy Vm) { qput(survey(vm)); }

// Reads while main changes the arrays: a stale or torn value is fine, a
// fault or an unchecked index is not.
thread_fn reader(vm: racy Vm, rounds: i64) {
    var total = 0;
    for r in rounds {
        let s = survey(vm);
        total += s.dicts + s.jumps;
    }
    qput(Seen { dicts: total, pushed: 0, jumps: 0, vals: 0, epoch: -1, tag: 0 });
}

fn main() {
    var code: Op[>..] = [];
    var shapes: Shape[>..<] = [];
    var vals: i64[>..<] = [];
    for i in 100 {
        code.push(Op.Push { v: i });
        code.push(Op.Add {});
        code.push(Op.Jump { to: i as i32, cond: i % 3 == 0 });
        shapes.push(Shape { id: i as i32, dict: i % 4 == 0, n: 7 });
        vals.push(i * 2);
    }
    var vm = Vm { code: code, shapes: shapes, vals: vals, epoch: 5, open: true,
                  tag: [1, 2, 3, 4] };
    let m = survey(vm);
    print("main:   ", m.dicts, " ", m.pushed, " ", m.jumps, " ", m.vals, " ", m.epoch, " ",
          m.tag);
    thread_wait(thread_spawn(worker, racy(vm)));
    let w, ok = qget<Seen>(0);
    print("worker: ", w.dicts, " ", w.pushed, " ", w.jumps, " ", w.vals, " ", w.epoch, " ",
          w.tag, " ", ok);
    let v = racy(vals);
    print("view of vals: ", v.len, " ", v[0], " ", v[99]);

    // Main keeps writing while two readers read. A reader may end on a
    // bounds check, a length that shrank under it: contained, that ends only
    // the reader, as a compile's failed check ends only that job.
    let a = thread_spawn_contained(reader, racy(vm), 200);
    let b = thread_spawn_contained(reader, racy(vm), 200);
    for round in 2000 {
        for i in 50 { vals.push(i); shapes.push(Shape { id: 0, dict: true, n: 1 }); }
        if round % 3 == 0 { vals.clear(); shapes.resize(10, Shape { id: 0, dict: false, n: 0 }); }
        for i in shapes.len { shapes[i].dict = !shapes[i].dict; }
        vm.epoch = round;
    }
    var why: u8[>..] = [];
    thread_wait(a, why);
    thread_wait(b, why);
    print("readers done");
}
