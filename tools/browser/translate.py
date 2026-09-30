#!/usr/bin/env python3
#
# translate.py -- compile the engine's sources for TessronOS (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
# gn describes the aarch64 build of Chromium's renderer (tools/browser/
# gngen.sh), for a Linux target and compilers TessronOS does not have. This
# takes the compile commands of the targets a program needs, keeps what each
# source asks for (its defines, include paths, language level and warnings)
# and compiles it with the TessronOS toolchain (aarch64-none-elf, newlib, the
# threads of lib/libpthread) against the port's shims, which give the Linux
# and POSIX surface the sources reach for.
#
#   translate.py --label //v8:v8_hello_world ...   the targets' objects
#   translate.py --filter REGEX                     sources by path
#
# Objects go to $OBJ/<the object path gn gives>, a source is compiled again
# only when it or its command changed, and a failure leaves <object>.fail
# with its first errors. --exclude-list names sources the port replaces or
# leaves out, one path fragment per line. A source with a patch in
# tools/browser/patches (a unified diff against the checkout, paths from
# its root) is compiled from a patched copy under $OBJ/_patched; the
# checkout itself is never changed.

import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import re
import shlex
import subprocess
import sys

HOME = os.path.expanduser('~')
HERE = os.path.dirname(os.path.abspath(__file__))
# this repository (tessron-chromium) and the TessronOS tree it is built into
BR = os.path.dirname(os.path.dirname(HERE))
TS = os.environ.get('TESSRONOS') or os.path.join(os.path.dirname(BR), 'TessronOS')

# Flags of the gn command line that belong to the compilers and system it
# was written for.
DROP_PREFIX = ('-fstack-protector', '-fPIC', '-fPIE', '-fpic', '-fpie', '-pthread',
               '-mbranch-protection', '-D_FORTIFY_SOURCE', '-march=', '-mcpu=', '-mtune=',
               '--sysroot', '-B', '-fsanitize', '-fcf-protection', '-Wl,', '-gsplit-dwarf',
               '-fdebug-compilation-dir', '-no-canonical-prefixes', '-mno-outline-atomics',
               '-moutline-atomics', '-fuse-ld', '-Werror', '-D_GLIBCXX_ASSERTIONS',
               '-D_LIBCPP_')
DROP_EXACT = ('-c', '-MD', '-MMD', '-pipe')
DROP_WITH_ARG = ('-o', '-MF', '-MT', '-MQ', '-target', '--target')


# The time the copies of the port's headers are given when they only
# gained names (local_includes)
INC_TIME = 1790000000


def local_includes(a):
    """The port's header directories copied next to the objects.

    A source reads hundreds of headers and each is looked for in these
    directories first; on the Windows drive every such look costs a round
    trip, so the compiler is pointed at a copy on the Linux side instead.

    A copy's time changes only when its text does (the times of the
    sources are not carried over), so that an object is made again only
    for a header that changed. With --additive the headers that changed
    are taken to have only gained names (declarations, constants): their
    copies keep the time of the unchanged ones, and the objects already
    made from them stay.
    """
    dst = os.path.join(a.obj, '_inc')
    for name, src in (('shim', a.shim),
                      ('tscxx', os.path.join(TS, 'include', 'tscxx')),
                      ('tsposix', os.path.join(TS, 'include', 'tsposix'))):
        d = os.path.join(dst, name)
        os.makedirs(d, exist_ok=True)
        r = subprocess.run(['rsync', '-r', '--delete', '--checksum', '--out-format=%n',
                            src + '/', d + '/'], check=True, capture_output=True, text=True)
        changed = [os.path.join(d, l) for l in r.stdout.splitlines()
                   if l and not l.endswith('/') and os.path.isfile(os.path.join(d, l))]
        for f in changed:
            print('translate: header changed: %s%s' % (
                os.path.relpath(f, dst), ' (names added only)' if a.additive else ''))
            if a.additive:
                os.utime(f, (INC_TIME, INC_TIME))
    return os.path.join(dst, 'shim'), os.path.join(dst, 'tscxx'), os.path.join(dst, 'tsposix')


INCS = None


def target_flags(shim):
    shim, tscxx, tsposix = INCS
    return [
        '-march=armv8.2-a', '-mtune=cortex-a76', '-mno-outline-atomics',
        '-fno-stack-protector', '-ffunction-sections', '-fdata-sections',
        '-D__linux__', '-D__linux', '-Dlinux', '-D__unix__', '-D__unix', '-Dunix',
        '-D_GNU_SOURCE', '-D_POSIX_TIMERS=1', '-D_POSIX_MONOTONIC_CLOCK=200112L',
        '-D_POSIX_PRIORITY_SCHEDULING=1', '-D_POSIX_CLOCK_SELECTION=200112L',
        '-D__TESSRONOS__=1', '-DABSL_FORCE_WAITER_MODE=2',
        '-I' + shim,
        '-I' + tscxx,
        '-I' + tsposix,
        '-include', os.path.join(shim, 'ts_compat.h'),
    ]


def parse_args():
    p = argparse.ArgumentParser()
    p.add_argument('--cr', default=os.path.join(HOME, 'cr6', 'src'))
    p.add_argument('--out', default='tessronos', help='gn output directory under $CR/out')
    p.add_argument('--obj', default=os.path.join(HOME, 'tessronos-obj'))
    p.add_argument('--label', action='append', default=[],
                   help='gn label whose objects are compiled (repeatable)')
    p.add_argument('--labels-file', help='file of gn labels, one per line')
    p.add_argument('--filter', help='regex on the source path')
    p.add_argument('--exclude-list', action='append', default=[],
                   help='files of path fragments left out (repeatable; exclude.txt always)')
    p.add_argument('--shim', default=os.path.join(BR, 'application', 'browser', 'port', 'shim'))
    p.add_argument('--xpack', default=os.environ.get(
        'TS_TOOLCHAIN', os.path.join(HOME, '.local', 'xPacks', 'aarch64-none-elf-gcc', 'bin')))
    p.add_argument('--jobs', type=int, default=4)
    p.add_argument('--list', action='store_true', help='print what would be compiled')
    p.add_argument('--objlist', help='write the list of objects here')
    p.add_argument('--only', help='file of object paths: of what is selected, compile only these')
    p.add_argument('--force', action='store_true')
    p.add_argument('--additive', action='store_true',
                   default=os.environ.get('TRANSLATE_ADDITIVE') == '1',
                   help='the port\'s headers that changed only gained names: '
                        'the objects made before stay current')
    p.add_argument('--unity', type=int, default=0,
                   help='compile up to N sources of the same command as one '
                        'translation unit (a source that does not build that way '
                        'is compiled alone)')
    p.add_argument('--extra', action='append', default=[],
                   help='SRC=REF: compile SRC with the command gn gives the '
                        'engine\'s source REF (a path ending); SRC is a path of '
                        'this repository, or of the checkout when it begins with '
                        '//; the object goes to obj/tessronos_port/<name of SRC>.o '
                        '(repeatable)')
    p.add_argument('--extras-file', help='file of SRC=REF lines')
    p.add_argument('--patches', default=os.path.join(HERE, 'patches'))
    p.add_argument('--flags-file', default=os.path.join(HERE, 'flags.txt'),
                   help='lines "PATH-FRAGMENT FLAG...": flags added for the sources it '
                        'names; {TS} stands for the TessronOS tree')
    return p.parse_args()


def patched_sources(a):
    """{source path relative to the checkout: patched copy} for every patch."""
    out = {}
    if not os.path.isdir(a.patches):
        return out
    root = os.path.join(a.obj, '_patched')
    for name in sorted(os.listdir(a.patches)):
        if not name.endswith('.patch'):
            continue
        pf = os.path.join(a.patches, name)
        targets = [l[4:].split('\t')[0].strip() for l in open(pf, encoding='utf-8')
                   if l.startswith('+++ ')]
        for t in targets:
            rel = t[2:] if t.startswith(('a/', 'b/')) else t
            src = os.path.join(a.cr, rel)
            dst = os.path.join(root, rel)
            stamp = dst + '.stamp'
            key = hashlib.sha1(open(pf, 'rb').read() + open(src, 'rb').read()).hexdigest()
            if not (os.path.exists(stamp) and open(stamp).read() == key):
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(src, 'rb') as f, open(dst, 'wb') as g:
                    g.write(f.read())
                subprocess.run(['patch', '-s', '-p1', '-d', root, '-i', pf], check=True)
                with open(stamp, 'w') as f:
                    f.write(key)
            out[rel] = dst
    return out


PATCHED = {}
FILE_FLAGS = []
ERRLINES = int(os.environ.get('TRANSLATE_ERRLINES', '6'))
RELAXED_WHEN = ('[-Wchanges-meaning]', '[-fpermissive]', '[-Wtemplate-body]')
RELAXED = ['-fpermissive', '-Wno-changes-meaning', '-Wno-template-body']


def load_file_flags(path):
    out = []
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.split('#', 1)[0].split()
            if len(line) >= 2:
                out.append((line[0], [f.replace('{TS}', TS) for f in line[1:]]))
    return out


def label_dirs(labels):
    """gn puts a target's objects in obj/<dir>/<name>/ (or obj/<dir>/<name>.<file>)."""
    out = []
    for l in labels:
        l = l.strip()
        if not l.startswith('//'):
            continue
        l = l.split('(')[0]
        d, _, name = l[2:].partition(':')
        if not name:
            name = os.path.basename(d)
        out.append('obj/%s/%s/' % (d, name) if d else 'obj/%s/' % name)
        out.append('obj/%s/%s.' % (d, name) if d else 'obj/%s.' % name)
    return out


def load_exclude(path):
    pats = []
    if path and os.path.exists(path):
        for line in open(path, encoding='utf-8'):
            line = line.split('#', 1)[0].strip()
            if line:
                pats.append(line)
    return pats


def translate(entry, a):
    cmd = shlex.split(entry['command'])
    src = entry['file']
    is_c = src.endswith('.c')
    is_asm = src.endswith(('.S', '.s'))
    toks = []
    out = None
    i = 1
    while i < len(cmd):
        t = cmd[i]
        if t in DROP_WITH_ARG:
            if t == '-o':
                out = cmd[i + 1]
            i += 2
            continue
        if t in DROP_EXACT or t == src or any(t.startswith(p) for p in DROP_PREFIX):
            i += 1
            continue
        toks.append(t)
        i += 1
    if out is None:
        return None
    rel = os.path.normpath(os.path.join(os.path.join(a.cr, 'out', a.out), src))
    rel = os.path.relpath(rel, a.cr)
    if rel in PATCHED:
        # quoted includes are looked for beside the original first
        toks = ['-iquote', os.path.dirname(os.path.join(a.cr, rel))] + toks
        src = PATCHED[rel]
    cc = os.path.join(a.xpack, 'aarch64-none-elf-gcc' if (is_c or is_asm) else 'aarch64-none-elf-g++')
    obj = os.path.join(a.obj, a.out, out)
    extra = [f for frag, fl in FILE_FLAGS if frag in entry['file'] for f in fl]
    argv = [cc] + target_flags(a.shim) + toks + extra + ['-c', src, '-o', obj]
    return obj, argv


def deps_current(obj, cwd):
    """Whether no file the object was made from (its .d) is newer than it."""
    try:
        t = os.path.getmtime(obj)
        text = open(obj + '.d').read().replace('\\\n', ' ')
    except OSError:
        return False
    for f in text.split(':', 1)[1].split():
        try:
            if os.path.getmtime(os.path.join(cwd, f)) > t:
                return False
        except OSError:
            return False
    return True


def compile_one(job):
    obj, argv, cwd, force = job
    stamp = obj + '.cmd'
    key = hashlib.sha1('\0'.join(argv).encode()).hexdigest()
    if not force and os.path.exists(obj) and os.path.exists(stamp):
        try:
            if open(stamp).read() == key and deps_current(obj, cwd):
                return obj, True, None
        except OSError:
            pass
    os.makedirs(os.path.dirname(obj), exist_ok=True)
    src = argv[argv.index('-c') + 1]
    argv = argv + ['-MD', '-MF', obj + '.d']
    r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True, errors='replace')
    if r.returncode != 0 and any(w in r.stderr for w in RELAXED_WHEN):
        # what gcc refuses and the compiler the sources are written for
        # accepts (a member named as the type it is declared with, and the
        # like): compiled once more with those errors made warnings
        r = subprocess.run(argv + RELAXED, cwd=cwd, capture_output=True, text=True,
                           errors='replace')
    fail = obj + '.fail'
    if r.returncode != 0:
        errs = [l for l in r.stderr.splitlines() if 'error' in l][:ERRLINES]
        with open(fail, 'w') as f:
            f.write('=== %s\n%s\n' % (src, '\n'.join(errs)))
        if os.path.exists(obj):
            os.remove(obj)
        return obj, False, '\n'.join(errs)
    if os.path.exists(fail):
        os.remove(fail)
    with open(stamp, 'w') as f:
        f.write(key)
    return obj, True, None


def is_current(obj, argv, cwd):
    stamp = obj + '.cmd'
    key = hashlib.sha1('\0'.join(argv).encode()).hexdigest()
    try:
        return (os.path.exists(obj) and open(stamp).read() == key
                and deps_current(obj, cwd))
    except OSError:
        return False


def unity_group_key(argv):
    """The command of a source without the source and the object."""
    i = argv.index('-c')
    return '\0'.join(argv[:i] + argv[i + 2:-2])


def unity_jobs(jobs, a):
    """Group the jobs into units of up to a.unity sources.

    Most of the time of one of the engine's sources goes to reading the
    headers it shares with its neighbours; sources of one target, compiled
    with one command, are included one after another into a generated unit
    so that the headers are read once. The grouping depends only on the list
    of sources, so a unit keeps its members from one run to the next. A unit
    whose members do not build together (names at file scope that clash)
    is marked and its sources are compiled one by one from then on.
    """
    udir = os.path.join(a.obj, a.out, 'obj', '_unity')
    os.makedirs(udir, exist_ok=True)
    patched_dir = os.path.join(a.obj, '_patched') + os.sep
    groups = {}
    single = []
    for j in jobs:
        obj, argv, cwd, force = j
        src = argv[argv.index('-c') + 1]
        # a patched copy stays in the group of its original (its command
        # is the original's with the original's directory to look in), so
        # that patching one source does not regroup its whole target
        key_argv = argv
        if src.startswith(patched_dir) and '-iquote' in argv:
            k = argv.index('-iquote')
            key_argv = argv[:k] + argv[k + 2:]
        # the generated bindings share names at file scope from one file
        # to the next, and never build together
        if (not src.endswith(('.cc', '.cpp'))
                or os.path.isabs(src) and not src.startswith(patched_dir)
                or '/renderer/bindings/' in src and src.startswith('gen/')):
            single.append(j)
            continue
        groups.setdefault(unity_group_key(key_argv), []).append(j)
    out = list(single)
    for key, members in groups.items():
        members.sort(key=lambda j: j[0])
        for k in range(0, len(members), a.unity):
            chunk = members[k:k + a.unity]
            if len(chunk) == 1:
                out.append(chunk[0])
                continue
            tag = hashlib.sha1('\n'.join(j[0] for j in chunk).encode()).hexdigest()[:16]
            base = os.path.join(udir, tag)
            if os.path.exists(base + '.nounity') or all(
                    is_current(j[0], j[1], j[2]) for j in chunk):
                out.extend(chunk)
                continue
            cwd = chunk[0][2]
            text = ''.join('#include "%s"\n' % os.path.normpath(
                os.path.join(cwd, j[1][j[1].index('-c') + 1])) for j in chunk)
            if not (os.path.exists(base + '.cc') and open(base + '.cc').read() == text):
                with open(base + '.cc', 'w') as f:
                    f.write(text)
            plain = [j for j in chunk if not j[1][j[1].index('-c') + 1].startswith(patched_dir)]
            argv = list((plain or chunk)[0][1])
            for j in chunk:
                if j[1][j[1].index('-c') + 1].startswith(patched_dir) and '-iquote' in j[1]:
                    k = j[1].index('-iquote')
                    if j[1][k + 1] not in argv:
                        argv[1:1] = ['-iquote', j[1][k + 1]]
            argv[argv.index('-c') + 1] = base + '.cc'
            argv[argv.index('-o') + 1] = base + '.o'
            out.append((base + '.o', argv, cwd, chunk[0][3], chunk))
    return out


def compile_unit(job):
    """A unit, or its members one by one when it does not build."""
    if len(job) == 4:
        return [compile_one(job)]
    obj, argv, cwd, force, members = job
    r = compile_one((obj, argv, cwd, force))
    if r[1]:
        # the members' own objects are left: a unit of another grouping
        # (the list of sources changed since) may still stand on them
        for m in members:
            if os.path.exists(m[0] + '.fail'):
                os.remove(m[0] + '.fail')
        return [r]
    open(obj[:-2] + '.nounity', 'w').write(r[2] or '')
    if os.path.exists(obj + '.fail'):
        os.remove(obj + '.fail')
    return [compile_one(m) for m in members]


def main():
    global INCS, PATCHED
    a = parse_args()
    INCS = local_includes(a)
    PATCHED = patched_sources(a)
    FILE_FLAGS[:] = load_file_flags(a.flags_file)
    cwd = os.path.join(a.cr, 'out', a.out)
    entries = json.load(open(os.path.join(cwd, 'compile_commands.json')))
    labels = list(a.label)
    if a.labels_file:
        labels += open(a.labels_file).read().split()
    prefixes = set(label_dirs(labels))

    def selected(o):
        # whether a prefix of the object path, up to a '/' or a '.', is a
        # label's directory
        for i, ch in enumerate(o):
            if ch in '/.' and o[:i + 1] in prefixes:
                return True
        return False
    fpat = re.compile(a.filter) if a.filter else None
    excl = []
    for f in [os.path.join(HERE, 'exclude.txt')] + a.exclude_list:
        excl += load_exclude(f)

    jobs = []
    seen = set()
    skipped = 0
    for e in entries:
        m = re.search(r'\s-o\s+(\S+)', e['command'])
        if not m or not m.group(1).startswith('obj/'):
            continue			# another toolchain's object
        o = m.group(1)
        if not e['file'].endswith(('.cc', '.cpp', '.c', '.S')):
            continue
        if prefixes and not selected(o):
            continue
        if fpat and not fpat.search(e['file']):
            continue
        if any(x in e['file'] for x in excl):
            skipped += 1
            continue
        t = translate(e, a)
        if t is None or t[0] in seen:
            continue
        seen.add(t[0])
        jobs.append((t[0], t[1], cwd, a.force))

    extras = list(a.extra)
    if a.extras_file:
        for line in open(a.extras_file):
            line = line.split('#', 1)[0].strip()
            if line:
                extras.append(line)
    for x in extras:
        src, _, ref = x.partition('=')
        if src.startswith('//'):
            src = os.path.join(a.cr, src[2:])
        elif not os.path.isabs(src):
            src = os.path.join(BR, src)
        e = next((e for e in entries if e['file'].endswith(ref)
                  and re.search(r'\s-o\s+obj/', e['command'])), None)
        if e is None:
            print('translate: no command for %s' % ref)
            return 1
        t = translate(dict(e, command=e['command'].replace(e['file'], src), file=src), a)
        name = os.path.splitext(os.path.basename(src))[0] + '.o'
        obj = os.path.join(a.obj, a.out, 'obj', 'tessronos_port', name)
        argv = t[1][:-1] + [obj]
        jobs.append((obj, argv, cwd, a.force))

    want = None
    if a.only:
        want = set(l.strip() for l in open(a.only) if l.strip())
        if a.unity <= 1 or a.list:
            jobs = [j for j in jobs if j[0] in want]
    print('translate: %d sources, %d left out' % (len(jobs), skipped))
    if a.list:
        for j in jobs:
            print(j[0])
        return 0
    if a.unity > 1:
        # units are made of the whole selection, so that they are the same
        # whatever --only picks of it
        jobs = unity_jobs(jobs, a)
        if want is not None:
            jobs = [j for j in jobs if j[0] in want or
                    (len(j) == 5 and any(m[0] in want for m in j[4]))]
        print('translate: %d units' % len(jobs))

    ok = bad = 0
    fails = []
    with cf.ThreadPoolExecutor(max_workers=a.jobs) as ex:
        for res in ex.map(compile_unit, jobs):
            for obj, good, err in res:
                if good:
                    ok += 1
                else:
                    bad += 1
                    fails.append((obj, err))
    if a.objlist:
        # what the program is made of: a unit's object, or its members'
        with open(a.objlist, 'w') as f:
            for j in jobs:
                if len(j) == 5 and not os.path.exists(j[0]):
                    for m in j[4]:
                        f.write(m[0] + '\n')
                else:
                    f.write(j[0] + '\n')
    print('translate: %d compiled or current, %d failed' % (ok, bad))
    counts = {}
    for obj, err in fails:
        for l in (err or '').splitlines()[:1]:
            k = re.sub(r'^[^ ]+:\d+:\d+: ', '', l)
            counts[k] = counts.get(k, 0) + 1
    for k, n in sorted(counts.items(), key=lambda x: -x[1])[:40]:
        print('%5d %s' % (n, k))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
