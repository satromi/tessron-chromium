#!/usr/bin/env python3
#
# nasm_objs.py -- the NASM sources of the engine for x86-64 (design 14.3)
#
# This software is distributed under the MIT License (LICENSE).
#
#   nasm_objs.py NASM CR OUT OBJDIR OBJLIST [JOBS]
#
# gn assembles the x86 SIMD routines of dav1d, FFmpeg, libaom,
# libjpeg-turbo, libvpx and OpenH264 with NASM in actions of its own
# (gn_run_binary.py clang_x64/nasm ...), which are not in
# compile_commands.json and so are not reached by translate.py. This reads
# those actions from $CR/out/$OUT/toolchain.ninja and runs each with the
# NASM of the toolchain (tools/x64/build_toolchain.sh nasm) instead of the
# checkout's, putting the object at OBJDIR/<the object path gn gives>.
# An object is assembled again when its source, a file it includes (its
# .d) or the command changed. OBJLIST gets the objects, one per line.
#

import concurrent.futures as cf
import hashlib
import os
import re
import shlex
import subprocess
import sys

NASM_RULE = re.compile(r'^rule (\S+)$')


def actions(ninja):
    """{rule: command template} of the NASM actions, and their builds."""
    rules = {}
    builds = []
    cur = None
    with open(ninja, encoding='utf-8') as f:
        for line in f:
            m = NASM_RULE.match(line.rstrip('\n'))
            if m:
                cur = m.group(1)
                continue
            if cur and line.startswith('  command = '):
                cmd = line[len('  command = '):].strip()
                if 'gn_run_binary.py clang_x64/nasm ' in cmd:
                    rules[cur] = cmd.split('gn_run_binary.py clang_x64/nasm ', 1)[1]
                cur = None
                continue
            if line.startswith('build ') and ': ' in line:
                head, _, rest = line[6:].partition(': ')
                parts = rest.split()
                if len(parts) >= 2 and parts[0] in rules:
                    builds.append((head.strip(), parts[0], parts[1]))
    return rules, builds


def deps_current(obj, d, cwd):
    try:
        t = os.path.getmtime(obj)
        text = open(d).read().replace('\\\n', ' ')
    except OSError:
        return False
    for f in text.split(':', 1)[1].split() if ':' in text else []:
        try:
            if os.path.getmtime(os.path.join(cwd, f)) > t:
                return False
        except OSError:
            return False
    return True


def assemble(job):
    nasm, cwd, obj, argv = job
    key = hashlib.sha1('\0'.join(argv).encode()).hexdigest()
    stamp = obj + '.cmd'
    d = obj + '.d'
    try:
        if open(stamp).read() == key and deps_current(obj, d, cwd):
            return obj, None
    except OSError:
        pass
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    r = subprocess.run([nasm] + argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    if r.returncode != 0:
        if os.path.exists(obj):
            os.remove(obj)
        return obj, (r.stderr.strip().splitlines() or ['failed'])[0]
    with open(stamp, 'w') as f:
        f.write(key)
    return obj, None


def main():
    nasm, cr, out, objdir, objlist = sys.argv[1:6]
    jobs = int(sys.argv[6]) if len(sys.argv) > 6 else 8
    cwd = os.path.join(cr, 'out', out)
    rules, builds = actions(os.path.join(cwd, 'toolchain.ninja'))
    work = []
    for o, rule, src in builds:
        name = os.path.splitext(os.path.basename(src))[0]
        t = rules[rule].replace('${source_name_part}', name).replace('${in}', src)
        argv = shlex.split(t)
        obj = os.path.join(objdir, o)
        # the object and its dependency file go beside the other objects
        k = argv.index('-o')
        argv[k + 1] = obj
        if '-MD' in argv:
            argv[argv.index('-MD') + 1] = obj + '.d'
        work.append((nasm, cwd, obj, argv))
    bad = 0
    with cf.ThreadPoolExecutor(max_workers=jobs) as ex:
        for obj, err in ex.map(assemble, work):
            if err:
                bad += 1
                print('nasm: %s: %s' % (os.path.relpath(obj, objdir), err))
    with open(objlist, 'w') as f:
        for w in work:
            if os.path.exists(w[2]):
                f.write(w[2] + '\n')
    print('nasm: %d sources, %d failed' % (len(work), bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
