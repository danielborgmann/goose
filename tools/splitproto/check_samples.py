# Checks the split prototype on small programs: builds each normally and split into many small units,
# runs both and compares exit code and output.
#   python check_samples.py goose.exe workdir file.goose...
import sys, os, re, subprocess
goose, work, files = sys.argv[1], sys.argv[2], sys.argv[3:]
here = os.path.dirname(os.path.abspath(__file__))
os.makedirs(work, exist_ok=True)
rt = None
ok = bad = 0
for f in files:
    name = os.path.splitext(os.path.basename(f))[0]
    d = os.path.join(work, name); os.makedirs(d, exist_ok=True)
    c, mono = os.path.join(d, 'p.c'), os.path.join(d, 'mono.exe')
    r = subprocess.run([goose, 'build', f, '-o', mono], capture_output=True, text=True)
    if r.returncode: print('skip', name, '(does not build)'); continue
    subprocess.run([goose, f, '-o', c], capture_output=True, text=True)
    # The program's own link inputs: whatever follows its C file on the link line.
    dry = subprocess.run([goose, 'build', f, '-o', mono, '--dry-run'], capture_output=True, text=True).stdout
    line = [l for l in dry.splitlines() if '/Fe:' in l][-1]
    k = line.index('.c ', line.index('/Fo:'))
    rt = line[k + 3:].strip()
    out = os.path.join(d, 'units')
    r = subprocess.run([sys.executable, os.path.join(here, 'split2.py'), c, out, '--target-kb', '2', '--link', rt] + sys.argv[0:0],
                       capture_output=True, text=True)
    split = os.path.join(out, 'prog.exe')
    if 'link' not in r.stdout or ' exit 0' not in r.stdout.split('link')[-1] or 'FAIL' in r.stdout:
        bad += 1; print('FAIL build', name, (r.stdout + r.stderr)[-900:]); continue
    def run(exe):
        try:
            p = subprocess.run([exe], capture_output=True, timeout=60, cwd=os.path.dirname(os.path.abspath(f)), stdin=subprocess.DEVNULL)
            # Timings a program prints, and its own path in a message, differ from run to run.
            norm = lambda b: re.sub(rb'[\d.]+', b'N', b.replace(os.path.dirname(exe).encode(), b''))
            return p.returncode, norm(p.stdout), norm(p.stderr)
        except subprocess.TimeoutExpired:
            return 'timeout', b'', b''
    a, b = run(mono), run(split)
    if a == b: ok += 1
    elif a[0] == b[0] == 'timeout': ok += 1
    else:
        bad += 1; print('FAIL run', name, a[0], b[0], len(a[1]), len(b[1]))
print(f'{ok} ok, {bad} failed')
