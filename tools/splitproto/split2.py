# Prototype of split C output: turns Goose's one generated C file into self-contained units that link.
#
# Each definition (a function body, a data object) is put in a unit chosen by a hash of its name, so
# an edit elsewhere does not move it. A unit holds the runtime header, then only the declarations its
# definitions reach by name (types, extern declarations of data and functions defined in other
# units), then its definitions. A unit whose text is unchanged keeps its object file.
#
#   python split2.py prog.c outdir [--target-kb 128] [--jobs J] [--opt /O2] [--link "<args>"] [-I dir]
import sys, os, re, time, subprocess, argparse, hashlib, concurrent.futures as cf

ap = argparse.ArgumentParser()
ap.add_argument('src'); ap.add_argument('out')
ap.add_argument('--target-kb', type=int, default=128)
ap.add_argument('--jobs', type=int, default=os.cpu_count())
ap.add_argument('--opt', default='/O2')
ap.add_argument('--cc', default=r'C:\Visual Studio\2022\Community\VC\Tools\Llvm\x64\bin\clang-cl.exe')
ap.add_argument('-I', dest='include', action='append', default=[])
ap.add_argument('--link', default=None, help='extra link inputs (objects, @rsp files); links out/prog.exe')
ap.add_argument('--stable-names', action='store_true', help='rename numbered data objects by content')
a = ap.parse_args()
T0 = time.perf_counter()

text = open(a.src, encoding='utf-8', newline='').read().replace('\r\n', '\n')

def cut(t, mark):
    i = t.index(mark)
    return t[:i], t[i + len(mark):]

runtime, rest = cut(text, '\n/* ---- types ---- */\n')
types, rest = cut(rest, '\n/* ---- data ---- */\n')
data, rest = cut(rest, '\n/* ==== runtime_ext.h ==== */\n')
ext, rest = cut(rest, '\n/* ---- includes ---- */\n')
includes, rest = cut(rest, '\n/* ---- extern prototypes ---- */\n')
externs, rest = cut(rest, '\n/* ---- prototypes ---- */\n')
protos, code = cut(rest, '\n/* ---- code ---- */\n')
types = types.replace('#pragma pack(push, 1)\n', '', 1)
assert types.rstrip().endswith('#pragma pack(pop)'), types[-80:]
types = types.rstrip()[:-len('#pragma pack(pop)')]

IDENT = re.compile(r'[A-Za-z_]\w*')

# The runtime keeps the thread's stack block and globals instance in static thread-locals, so every
# unit has its own. They are set as a thread program starts and never after, so the unit that sets them
# hands them to all the others there. (Declaring them extern instead makes clang take minutes over a
# large function.)
runtime = '#pragma clang diagnostic ignored "-Wunused-function"\n' + runtime
SYNC = re.compile(r'^(    )(gs_gl = [^\n]*;|gs_stack_block\([^\n]*\);|gs_rt_init\([^\n]*\);)$', re.M)
code = SYNC.sub(r'\1\2\n\1gs_split_sync(gs_stks, gs_nstks, gs_gl);', code)
protos += 'void gs_split_sync(gs_stack *p0, int64_t p1, void *p2);\n'

LIT = re.compile(r'''"(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|/\*.*?\*/''')

def items_of(t):
    """Top-level items: a preprocessor line, or text up to a line ending the item at brace depth 0."""
    out, cur, depth = [], [], 0
    for line in t.split('\n'):
        if not cur and not line.strip(): continue
        if not cur and line.startswith('#'):
            out.append(line + '\n'); continue
        cur.append(line)
        bare = LIT.sub('', line) if ('"' in line or "'" in line or '/*' in line) else line
        depth += bare.count('{') - bare.count('}')
        if depth == 0 and (line.endswith(';') or line == '}'):
            out.append('\n'.join(cur) + '\n'); cur = []
    assert not cur, cur[:3]
    return out

FN_HEAD = re.compile(r'^(?:static )?(?:GS_INLINE |GS_NOINLINE )?[^\n;={]*?\b([A-Za-z_]\w*)\(')

class Item:
    __slots__ = ('text', 'names', 'kind', 'decl', 'defn', 'deps', 'order')

def head_name(decl):
    """The name a data declaration declares: the last identifier before any array bounds."""
    d = decl
    while d.rstrip().endswith(']'): d = d[:d.rindex('[')]
    return [w for w in IDENT.findall(d) if not re.fullmatch(r'GS_[A-Z0-9_]+', w)][-1]

def classify(text, section):
    it = Item(); it.text = text; it.decl = it.defn = None
    first = text.split('\n', 1)[0]
    if text.startswith('#define '):
        it.kind = 'macro'; it.names = [IDENT.findall(text)[1]]
    elif text.startswith('#'):
        it.kind = 'always'; it.names = []
    elif text.startswith(('typedef', 'struct', 'enum', 'union')):
        it.kind = 'type'
        body = text.rstrip().rstrip(';')
        names = []
        m = re.match(r'(?:typedef )?(?:struct|enum|union) ([A-Za-z_]\w*)', text)
        if m: names.append(m.group(1))
        if text.startswith('typedef'):
            fp = re.search(r'\(\s*\*\s*([A-Za-z_]\w*)\s*\)', body) if body.endswith(')') else None
            names.append(fp.group(1) if fp else head_name(body))
        if text.startswith('enum') or 'enum {' in first or re.match(r'typedef enum', text):
            inner = text[text.index('{') + 1:text.rindex('}')] if '{' in text else ''
            names += [IDENT.match(p.strip()).group(0) for p in inner.split(',') if IDENT.match(p.strip())]
        it.names = names
    else:
        m = FN_HEAD.match(first)
        isfn = m and (text.rstrip().endswith('}') or (text.rstrip().endswith(');') and '=' not in first))
        if isfn:
            it.names = [m.group(1)]
            head = re.sub(r'^static (?:GS_INLINE )?', '', first)
            if text.rstrip().endswith('}'):
                it.kind = 'fn'
                it.decl = head[:head.rindex('{')].rstrip() + ';\n'
                it.defn = head + '\n' + text.split('\n', 1)[1]
            else:
                it.kind = 'proto'; it.text = head + ('\n' + text.split('\n', 1)[1] if '\n' in text.rstrip() else '\n')
                if not it.text.endswith('\n'): it.text += '\n'
        else:
            it.kind = 'data'
            body = re.sub(r'^static ', '', text)
            eq = body.find(' = ')
            brace = body.find('{')
            decl = body[:eq] if eq >= 0 and (brace < 0 or eq < brace) else body.rstrip().rstrip(';')
            it.names = [head_name(decl)]
            it.decl = 'extern ' + decl.rstrip() + ';\n'
            it.defn = body
    return it

sections = [('types', types), ('data', data), ('externs', externs), ('protos', protos), ('code', code)]
allitems = []
for sec, t in sections:
    for x in items_of(t):
        it = classify(x, sec); it.order = len(allitems); allitems.append(it)

if a.stable_names:
    # gs_str12 and the like are numbered by first use, so one new literal renames every later one.
    ren = {}
    NUM = re.compile(r'\b(gs_(?:str|strv|blob|file|loc))(\d+)\b')
    def apply(t): return NUM.sub(lambda m: ren.get(m.group(0), m.group(0)), t)
    for it in allitems:
        if it.kind == 'data' and NUM.fullmatch(it.names[0]):
            old = it.names[0]
            body = apply(it.defn).replace(old, '@')
            ren[old] = NUM.fullmatch(old).group(1) + '_' + hashlib.blake2b(body.encode(), digest_size=6).hexdigest()
    seen = set(); kept = []
    for it in allitems:
        it.text = apply(it.text); it.names = [ren.get(n, n) for n in it.names]
        if it.decl: it.decl = apply(it.decl)
        if it.defn: it.defn = apply(it.defn)
        if it.kind == 'data' and it.names[0] in seen and it.names[0].startswith('gs_'): continue
        if it.kind == 'data': seen.add(it.names[0])
        kept.append(it)
    allitems = kept

byname = {}
for it in allitems:
    for n in it.names: byname.setdefault(n, []).append(it)
# A function with a prototype and a body: the body's item carries both.
defined = {it.names[0] for it in allitems if it.kind == 'fn'}
allitems = [it for it in allitems if not (it.kind == 'proto' and it.names[0] in defined)]
for n in defined: byname[n] = [it for it in byname[n] if it.kind == 'fn']

known = set(byname)
for it in allitems:
    src = it.defn if it.kind in ('fn', 'data') else it.text
    it.deps = {w for w in set(IDENT.findall(src)) if w in known} - set(it.names)
extdeps = {w for w in set(IDENT.findall(ext + includes)) if w in known}

# What a declaration alone needs: for a function or data object, the names in its declaration.
decldeps = {}
for it in allitems:
    if it.decl: decldeps[id(it)] = {w for w in set(IDENT.findall(it.decl)) if w in known} - set(it.names)

defs = [it for it in allitems if it.kind in ('fn', 'data')]
total = sum(len(it.defn) for it in defs)
nb = 1
while nb * a.target_kb * 1000 < total: nb *= 2
def bucket(it):
    if len(it.defn) > a.target_kb * 1000: return 'big_' + it.names[0]
    base = re.sub(r'_\d+$', '', it.names[0])
    return 'u%04d' % (int.from_bytes(hashlib.blake2b(base.encode(), digest_size=4).digest(), 'little') % nb)
units = {}
for it in defs: units.setdefault(bucket(it), []).append(it)

def unit_text(name, mine):
    mineset = {id(it) for it in mine}
    need, work = {}, []
    def want(n, full):
        for it in byname.get(n, ()):
            f = full or it.kind not in ('fn', 'data')
            if need.get(id(it)) is None or (f and not need[id(it)][1]):
                need[id(it)] = (it, f); work.append((it, f))
    for it in mine:
        need[id(it)] = (it, True); work.append((it, True))
    for n in extdeps: want(n, False)
    while work:
        it, full = work.pop()
        for n in (it.deps if full else decldeps[id(it)]): want(n, False)
    chosen = sorted((v for v in need.values()), key=lambda v: v[0].order)
    out = [runtime, '\n#pragma pack(push, 1)\n']
    out += [it.text for it, _ in chosen if it.kind == 'type']
    out.append('#pragma pack(pop)\n')
    # Declarations of everything first, so definitions may come in any order.
    out += [it.decl for it, _ in chosen if it.kind in ('fn', 'data')]
    out += [it.text for it, _ in chosen if it.kind in ('macro', 'always', 'proto')]
    out += [ext, includes]
    out += [it.defn for it, full in chosen if full and id(it) in mineset and it.kind == 'data']
    out += [it.defn for it, full in chosen if full and id(it) in mineset and it.kind == 'fn']
    out.append('void gs_split_set_%s(gs_stack *s, int64_t n, void *gl) { gs_stks = s; gs_nstks = n; gs_gl = gl; }\n' % name)
    if name == 'main':
        for u in sorted(units): out.append('void gs_split_set_%s(gs_stack *s, int64_t n, void *gl);\n' % u)
        out.append('void gs_split_sync(gs_stack *s, int64_t n, void *gl) {\n' +
                   ''.join('    gs_split_set_%s(s, n, gl);\n' % u for u in sorted(units)) + '}\n')
    return ''.join(out)

# The unit with main also owns the runtime's state.
mainunit = next(k for k, v in units.items() if any(it.names[0] == 'main' for it in v))
units['main'] = units.pop(mainunit)

os.makedirs(a.out, exist_ok=True)
jobs, objs, sizes = [], [], []
for name in sorted(units):
    t = unit_text(name, units[name])
    h = hashlib.blake2b((a.opt + t).encode(), digest_size=8).hexdigest()
    c, o = os.path.join(a.out, f'{name}-{h}.c'), os.path.join(a.out, f'{name}-{h}.obj')
    objs.append(o); sizes.append(len(t))
    if not os.path.exists(o):
        open(c, 'w', encoding='utf-8', newline='\n').write(t)
        jobs.append((c, o))
keep = {os.path.basename(o)[:-4] for o in objs}
for f in os.listdir(a.out):
    if f.endswith(('.c', '.obj')) and f.rsplit('.', 1)[0] not in keep: os.remove(os.path.join(a.out, f))
T1 = time.perf_counter()
print(f'{len(defs)} definitions in {len(units)} units ({nb} buckets), {sum(sizes)/1e6:.1f} MB of C in all, '
      f'largest {max(sizes)/1e6:.2f} MB; split took {T1-T0:.1f} s; {len(jobs)} to compile')

def build(job):
    c, o = job
    t = time.perf_counter()
    cmd = [a.cc, '/nologo', a.opt, '/c', '-w'] + [f'/I{d}' for d in a.include] + [c, '/Fo:' + o + '.tmp']
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode == 0: os.replace(o + '.tmp', o)
    return c, time.perf_counter() - t, r.returncode, (r.stdout + r.stderr)[:600]

fails = 0
times = []
with cf.ThreadPoolExecutor(a.jobs) as ex:
    for c, dt, rc, out in ex.map(build, sorted(jobs, key=lambda j: -os.path.getsize(j[0]))):
        times.append((dt, os.path.basename(c)))
        if rc:
            fails += 1
            if fails <= 4: print('FAIL', os.path.basename(c), out)
T2 = time.perf_counter()
times.sort(reverse=True)
if times:
    print(f'compile wall {T2-T1:.1f} s, {a.jobs} jobs, cpu {sum(t for t, _ in times):.0f} s, {fails} failed; slowest: '
          + ', '.join(f'{n.split("-")[0]} {t:.1f}' for t, n in times[:4]))
if a.link is not None and not fails:
    rsp = os.path.join(a.out, 'objs.rsp')
    open(rsp, 'w').write('\n'.join('"' + o.replace('\\', '/') + '"' for o in objs))
    cmd = f'"{a.cc}" /nologo /Fe:"{os.path.join(a.out, "prog.exe")}" @"{rsp}" {a.link}'
    r = subprocess.run(cmd, capture_output=True, text=True)
    print(f'link {time.perf_counter()-T2:.1f} s exit {r.returncode}', (r.stdout + r.stderr)[:1500])
