#!/usr/bin/env python3
"""mini-os32 compiler test runner.

Each tests/NN_name.c is compiled with the in-OS compiler and run. Its stdout is
compared with tests/NN_name.expected. Directives in the first lines of a test:

    // FILES: a.c b.c          extra source files (from tests/ or fs/) to link
    // EXPECT: compile-error   the test passes iff the compiler rejects it

Modes:
    --host   (default) gcc-built compiler harness (tools/cc_host) + Unicorn
             CPU emulation of the produced OS binary (tools/osrun.py).
    --qemu   boot build/disk.img in qemu-system-i386 with the tests placed in
             the image's filesystem, drive the shell over the QEMU monitor,
             read results from the serial console. This is the ground truth.

Exit status is non-zero if any test fails. --markdown prints a results table.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, 'tests')
FS = os.path.join(ROOT, 'fs', 'lib')
TOOLS = os.path.join(ROOT, 'tools')

def parse_directives(path):
    files, expect = [], 'run'
    with open(path) as f:
        for line in f:
            if not line.startswith('//'): break
            m = re.match(r'//\s*FILES:\s*(.*)', line)
            if m: files = m.group(1).split()
            m = re.match(r'//\s*EXPECT:\s*(\S+)', line)
            if m: expect = m.group(1)
    return files, expect

def load_tests(names):
    tests = []
    for fn in sorted(os.listdir(TESTS)):
        if not re.match(r'\d\d_.*\.c$', fn): continue
        base = fn[:-2]
        if base.endswith('_b'): continue  # helper translation unit
        if names and not any(n in base for n in names): continue
        files, expect = parse_directives(os.path.join(TESTS, fn))
        exp_path = os.path.join(TESTS, base + '.expected')
        expected = open(exp_path).read() if os.path.exists(exp_path) else None
        tests.append(dict(name=base, src=fn, files=files, expect=expect, expected=expected))
    return tests

def build_host_tools():
    cc_host = os.path.join(TOOLS, 'host', 'hostcc0')
    if not os.path.exists(cc_host) or os.path.getmtime(os.path.join(ROOT, 'user', 'cc.c')) > os.path.getmtime(cc_host):
        subprocess.check_call(['sh', os.path.join(TOOLS, 'host', 'build.sh')], stdout=subprocess.DEVNULL)
    return cc_host

def stage_dir(test, d):
    """Copy libc sources/headers plus this test's files into a scratch dir."""
    os.makedirs(os.path.join(d, 'lib'))
    for fn in os.listdir(FS):
        if fn.endswith('.c') or fn.endswith('.h'):
            shutil.copy(os.path.join(FS, fn), os.path.join(d, 'lib'))
    for fn in os.listdir(TESTS):
        if fn.endswith('.h'): shutil.copy(os.path.join(TESTS, fn), d)
    shutil.copy(os.path.join(TESTS, test['src']), d)
    for extra in test['files']:
        for cand in (os.path.join(TESTS, extra), os.path.join(FS, extra)):
            if os.path.exists(cand): shutil.copy(cand, d); break

def run_host(test, cc_host, verbose):
    d = tempfile.mkdtemp(prefix='cctest_')
    stage_dir(test, d)
    out = test['name'][:15]
    srcs = ' '.join([test['src']] + test['files'])
    p = subprocess.run([cc_host], input=f'{srcs}\n{out}\n', capture_output=True, text=True, cwd=d, timeout=60)
    compiled = p.returncode == 0 and os.path.exists(os.path.join(d, out))
    p.stdout = p.stdout.replace('cc v8 (relative paths)\n', '')
    if test['expect'] == 'compile-error':
        ok = not compiled
        detail = p.stdout.strip().splitlines()[-2:] if not compiled else ['compiled successfully (unexpected)']
        shutil.rmtree(d); return ok, '\n'.join(detail)
    if not compiled:
        shutil.rmtree(d); return False, 'compile failed: ' + p.stdout.strip()
    r = subprocess.run([sys.executable, os.path.join(TOOLS, 'osrun.py'), out], capture_output=True, text=True, cwd=d, timeout=60)
    actual = r.stdout
    ok = actual == test['expected']
    shutil.rmtree(d)
    return ok, actual if ok else f'expected:\n{test["expected"]}actual:\n{actual}{r.stderr}'

def run_qemu(tests, verbose):
    """Batch tests into disk images (the FS directory holds 32 entries)."""
    sys.path.insert(0, TOOLS)
    import qemu_drive
    results = {}
    headers = [fn for fn in os.listdir(TESTS) if fn.endswith('.h')]
    i = 0
    BATCH = 400   # the directory holds 512 entries
    while i < len(tests):
        batch = tests[i:i + BATCH]; i += BATCH
        fsdir = tempfile.mkdtemp(prefix='ccfs_')
        shutil.copytree(os.path.join(ROOT, 'fs'), fsdir, dirs_exist_ok=True)
        for fn in headers: shutil.copy(os.path.join(TESTS, fn), fsdir)
        for t in batch:
            shutil.copy(os.path.join(TESTS, t['src']), fsdir)
            for extra in t['files']:
                p = os.path.join(TESTS, extra)
                if os.path.exists(p): shutil.copy(p, fsdir)
        img = os.path.join(fsdir, 'disk.img')
        subprocess.check_call([os.path.join(TOOLS, 'mkimage.sh'), fsdir, img], cwd=ROOT, stdout=subprocess.DEVNULL)
        tmp = tempfile.mkdtemp(prefix='qemu_')
        ses = qemu_drive.Session(img, os.path.join(tmp, 'serial.log'), os.path.join(tmp, 'mon.sock'))
        if not ses.wait_for('/> ', 30):
            raise SystemExit('boot failed')
        crashed = False
        for t in batch:
            out = t['name'][:15]
            srcs = ' '.join([t['src']] + t['files'])
            if crashed:
                results[t['name']] = (False, 'not run: OS crashed earlier in this batch'); continue
            mark = len(ses.serial())
            ses.send_line('cc', wait_prompt='source files: ')
            ses.send_line(srcs, wait_prompt='output file: ')
            if not ses.send_line(out, wait_prompt='/> ', timeout=120):
                crashed = True
                results[t['name']] = (False, 'OS crashed or hung during compile:\n' + ses.serial()[mark:][-300:]); continue
            comp = ses.serial()[mark:]
            compiled = '\nok\n' in comp
            if t['expect'] == 'compile-error':
                results[t['name']] = (not compiled, comp.strip().splitlines()[-2:] if not compiled else ['compiled (unexpected)'])
                if compiled: ses.send_line('rm ' + out)
                continue
            if not compiled:
                results[t['name']] = (False, 'compile failed: ' + comp.strip()); continue
            mark = len(ses.serial())
            if not ses.send_line('run ' + out, wait_prompt='/> ', timeout=120):
                crashed = True
                results[t['name']] = (False, 'OS crashed or hung while running:\n' + ses.serial()[mark:][-300:]); continue
            s = ses.serial()[mark:]
            # transcript: "run NAME\n<program output>exit N\n/> "
            body = s.split('\n', 1)[1] if '\n' in s else ''
            # transcript ends with the next prompt; older shells also printed "exit N"
            body = re.sub(r'(exit \d+\n)?/> $', '', body)
            actual = body
            ok = actual == t['expected']
            results[t['name']] = (ok, actual if ok else f'expected:\n{t["expected"]}actual:\n{actual}')
            ses.send_line('rm ' + out)
        ses.close()
        shutil.rmtree(tmp); shutil.rmtree(fsdir)
    return results

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--qemu', action='store_true')
    ap.add_argument('--markdown', action='store_true')
    ap.add_argument('-v', action='store_true')
    ap.add_argument('names', nargs='*')
    a = ap.parse_args()
    tests = load_tests(a.names)
    if a.qemu:
        results = run_qemu(tests, a.v)
    else:
        cc_host = build_host_tools()
        results = {t['name']: run_host(t, cc_host, a.v) for t in tests}
    npass = nfail = nknown = 0
    rows = []
    for t in tests:
        ok, detail = results[t['name']]
        known = t['expect'] == 'known-bug'
        if known:
            tag = 'KNOWN-BUG' if not ok else 'FIXED?'   # a known bug that now passes deserves a look
            nknown += 1
        else:
            tag = 'PASS' if ok else 'FAIL'
            npass += ok; nfail += (not ok)
        print(f'{tag:9s} {t["name"]}')
        if (not ok and not known) or a.v:
            print('    ' + str(detail).replace('\n', '\n    ')[:600])
        rows.append((t['name'], t['expect'], ok))
    print(f'\n{npass} passed, {nfail} failed, {nknown} known bugs documented ({"qemu" if a.qemu else "host"} mode)')
    if a.markdown:
        print('\n| test | kind | result |\n|---|---|---|')
        kind = {'compile-error': 'must reject', 'known-bug': 'known bug', 'run': 'compile+run'}
        for n, e, ok in rows:
            res = 'pass' if ok else ('still broken' if e == 'known-bug' else 'FAIL')
            print(f'| `{n}` | {kind[e]} | {res} |')
    sys.exit(0 if nfail == 0 else 1)

if __name__ == '__main__':
    main()
