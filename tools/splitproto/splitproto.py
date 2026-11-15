# Prototype: what would splitting Goose's one C file into units buy?
#
# Splits the code section of a generated C file at function boundaries into N units, each with the
# whole prelude (runtime, types, data, prototypes) in front, and compiles them in parallel. The units
# do not link (every function is still `static`); this only measures compile time per unit and in all.
#
#   python splitproto.py prog.c outdir [--units N] [--jobs J] [--opt /O2]
import sys, os, time, subprocess, argparse, concurrent.futures as cf

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('out')
ap.add_argument('--units', type=int, default=64)
ap.add_argument('--jobs', type=int, default=os.cpu_count())
ap.add_argument('--opt', default='/O2')
ap.add_argument('--cc', default=r'C:\Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe')
ap.add_argument('--include', action='append', default=[])
a = ap.parse_args()

text = open(a.src, encoding='utf-8', newline='').read().replace('\r\n', '\n')
mark = '\n/* ---- code ---- */\n'
cut = text.index(mark) + len(mark)
prelude, code = text[:cut], text[cut:]

# A static function nothing in its unit calls is dropped before it costs anything, so the functions
# get external linkage here, as a real split would give them.
import re
def unstatic(t):
    t = re.sub(r'^static GS_INLINE (?=[^\n;=]*\()', '', t, flags=re.M)
    t = re.sub(r'^static GS_NOINLINE (?=[^\n;=]*\()', 'GS_NOINLINE ', t, flags=re.M)
    return re.sub(r'^static (?=[^\n;=]*\()', '', t, flags=re.M)
pm = prelude.index('/* ---- prototypes ---- */')
prelude = prelude[:pm] + unstatic(prelude[pm:])
code = unstatic(code)

# A top-level item ends at a line that is exactly "}" (bodies) or at a one-line declaration.
items, cur = [], []
for line in code.split('\n'):
    cur.append(line)
    if line == '}' or (len(cur) == 1 and line.endswith(';') and not line.startswith(' ')):
        items.append('\n'.join(cur) + '\n'); cur = []
tail = '\n'.join(cur)
total = sum(len(i) for i in items)
per = total / a.units
units, buf, size = [], [], 0
for it in items:
    buf.append(it); size += len(it)
    if size >= per and len(units) < a.units - 1:
        units.append(''.join(buf)); buf, size = [], 0
units.append(''.join(buf) + tail)

os.makedirs(a.out, exist_ok=True)
paths = []
for i, u in enumerate(units):
    p = os.path.join(a.out, f'u{i:03d}.c')
    open(p, 'w', encoding='utf-8', newline='\n').write(prelude + u)
    paths.append(p)
print(f'{len(items)} items, {len(units)} units, prelude {len(prelude)/1e6:.1f} MB, code {total/1e6:.1f} MB')

def build(p):
    t = time.perf_counter()
    cmd = [a.cc, '/nologo', a.opt, '/c', '-w'] + [f'/I{d}' for d in a.include] + [p, '/Fo:' + p[:-2] + '.obj']
    r = subprocess.run(cmd, capture_output=True, text=True)
    return p, time.perf_counter() - t, r.returncode, (r.stdout + r.stderr)[-400:]

t0 = time.perf_counter()
times = []
with cf.ThreadPoolExecutor(a.jobs) as ex:
    for p, dt, rc, out in ex.map(build, paths):
        times.append(dt)
        if rc: print('FAIL', os.path.basename(p), out)
wall = time.perf_counter() - t0
times.sort()
print(f'wall {wall:.1f} s with {a.jobs} jobs; per unit min {times[0]:.1f} median {times[len(times)//2]:.1f} max {times[-1]:.1f}; sum {sum(times):.0f} s')
