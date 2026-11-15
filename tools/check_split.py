# Checks `goose build --split` against the ordinary build: builds each program both ways, runs both
# and compares exit code and output (numbers apart, since programs print timings). Programs that
# import gfx, ui or audio are only built.
#   python tools/check_split.py goose.exe workdir [-j N] [--kb N] file.goose...
# --kb sets GOOSE_SPLIT_KB (default 2: tiny units, so nearly every call crosses a unit).
import sys, os, re, subprocess
from concurrent.futures import ThreadPoolExecutor

args = sys.argv[1:]
def opt(name, default):
    if name in args:
        i = args.index(name); v = args[i + 1]; del args[i:i + 2]; return v
    return default
jobs, kb = int(opt('-j', '8')), opt('--kb', '2')
goose, work, files = os.path.abspath(args[0]), os.path.abspath(args[1]), args[2:]
env = dict(os.environ, GOOSE_SPLIT_KB=kb)

def run(exe, cwd):
    try:
        p = subprocess.run([exe], capture_output=True, timeout=120, cwd=cwd, stdin=subprocess.DEVNULL)
    except subprocess.TimeoutExpired:
        return ('timeout', b'', b'')
    norm = lambda b: re.sub(rb'[\d.]+', b'N', b.replace(os.path.dirname(exe).encode(), b''))
    return (p.returncode, norm(p.stdout), norm(p.stderr))

def one(f):
    f = os.path.abspath(f)
    name = os.path.splitext(os.path.basename(f))[0]
    d = os.path.join(work, name); os.makedirs(d, exist_ok=True)
    mono, split = os.path.join(d, 'mono.exe'), os.path.join(d, 'split.exe')
    r = subprocess.run([goose, 'build', f, '-o', mono], capture_output=True, text=True, env=env)
    if r.returncode: return ('skip', name, '')
    r = subprocess.run([goose, 'build', f, '--split', '-o', split], capture_output=True, text=True, env=env)
    if r.returncode: return ('FAIL build', name, (r.stdout + r.stderr)[-1500:])
    # A program that opens a window or plays sound is built both ways and not run: it would sit
    # on the screen, in the foreground, until the timeout.
    if re.search(r'^import (gfx|ui|audio);', open(f, encoding='utf-8').read(), re.M): return ('built only', name, '')
    a, b = run(mono, os.path.dirname(f)), run(split, os.path.dirname(f))
    if a == b: return ('ok', name, '')
    return ('FAIL run', name, f'exit {a[0]} vs {b[0]}, stdout {len(a[1])} vs {len(b[1])} bytes')

counts = {}
with ThreadPoolExecutor(jobs) as ex:
    for what, name, detail in ex.map(one, files):
        counts[what] = counts.get(what, 0) + 1
        if what.startswith('FAIL'): print(what, name, detail, flush=True)
print(', '.join(f'{n} {w}' for w, n in sorted(counts.items())))
sys.exit(1 if any(w.startswith('FAIL') for w in counts) else 0)
