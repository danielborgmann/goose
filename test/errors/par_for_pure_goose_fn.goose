// error: pure is for an extern fn: a Goose function's effects are checked
import std;
extern pure fn ok(x: i32) -> i32 = "abs";
checkfree pure fn f(x: i32) -> i32 { return x; }

fn main() { print(ok(f(1))); }
