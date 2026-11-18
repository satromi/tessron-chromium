#!/usr/bin/env python3
#
# rust_hand.py -- Rust libraries of the checkout built by hand (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   rust_hand.py OUT-DIR NINJA OUTPUT...
#
# Each output named (an rlib of the build graph), and every rlib, static
# library and build script's output it stands on that is not there yet,
# is made by its own edge of the graph, in the order they need: the
# command `ninja -t commands -s` gives for it, run in OUT-DIR, after the
# response file ninja would write (rust_edge.py's way). The static
# libraries are the C++ halves of the bridges, which the engine's link
# takes from its own translation: an empty archive stands for each.
#
# ninja itself is not run to build: it would rebuild whatever its log
# does not know, actions that write generated headers among them, and a
# header written again makes the engine's translation of everything that
# includes it start over (tools/browser/translate.py looks at times).
# Outputs that are already there are kept.

import os
import re
import subprocess
import sys

out_dir, ninja = sys.argv[1], sys.argv[2]
wanted = sys.argv[3:]
env = dict(os.environ)


def unescape(v):
    return re.sub(r'\$(.)', lambda m: m.group(1), v)


def query(path):
    """(rule, explicit inputs, implicit inputs, order-only inputs), rule None for a source"""
    r = subprocess.run([ninja, '-C', out_dir, '-t', 'query', path], capture_output=True, text=True, errors='replace')
    rule, ex, im, oo = None, [], [], []
    state = None
    for line in r.stdout.splitlines():
        m = re.match(r'^  input: (\S+)', line)
        if m:
            rule = m.group(1)
            state = 'in'
            continue
        if line.startswith('  outputs:'):
            state = None
            continue
        if state == 'in' and line.startswith('    '):
            s = line[4:]
            if s.startswith('|| '):
                oo.append(s[3:])
            elif s.startswith('| '):
                im.append(s[2:])
            else:
                ex.append(s)
    return rule, ex, im, oo


def ninja_file_of(path):
    """The target's .ninja that has the build statement of path"""
    d = os.path.dirname(path)
    cands = [d + '.ninja'] + [os.path.join(d, f) for f in sorted(os.listdir(os.path.join(out_dir, d)))
                              if f.endswith('.ninja')] if os.path.isdir(os.path.join(out_dir, d)) else [d + '.ninja']
    pat = 'build ' + path.replace(':', '$:') + ' '
    for c in cands:
        full = os.path.join(out_dir, c)
        if not os.path.isfile(full):
            continue
        with open(full, errors='replace') as f:
            for line in f:
                if line.startswith(pat):
                    return c
    return None


def variables(ninja_rel, path):
    """The file's variables and those of path's build statement"""
    var = {}
    inside = False
    pat = 'build ' + path.replace(':', '$:') + ' '
    for line in open(os.path.join(out_dir, ninja_rel), errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('build '):
            inside = line.startswith(pat)
            continue
        m = re.match(r'^([a-z_]+) = (.*)$', line)
        if m:
            var[m.group(1)] = unescape(m.group(2))
            continue
        m = re.match(r'^  ([a-z_]+) = (.*)$', line)
        if m and inside:
            var[m.group(1)] = unescape(m.group(2))
    return var


def command(path):
    r = subprocess.run([ninja, '-C', out_dir, '-t', 'commands', '-s', path], capture_output=True, text=True, errors='replace')
    lines = [l for l in r.stdout.splitlines() if l.strip()]
    return lines[-1] if lines else ''


done = set()


def ensure(path, depth=0):
    if path in done or path.startswith('../../') or path.startswith('local_rustc_sysroot/'):
        return
    done.add(path)
    rule, ex, im, oo = query(path)
    if rule is None:
        return
    if rule == 'phony':
        for i in ex + im:
            if not i.startswith('../../'):
                ensure(i, depth + 1)
        return
    if os.path.exists(os.path.join(out_dir, path)):
        return
    for i in ex + im + oo:
        if i.startswith('../../'):
            continue
        if i.endswith(('.rlib', '.a', '.so', '.rmeta')) or i.startswith('phony/') or 'build_script' in i \
                or i.startswith('gen/'):
            ensure(i, depth + 1)
    print('%sbuild %s (%s)' % ('  ' * depth, path, rule), flush=True)
    if rule == 'alink':
        with open(os.path.join(out_dir, path), 'wb') as f:
            f.write(b'!<arch>\n')
        return
    if rule.startswith('rust_'):
        nf = ninja_file_of(path)
        if nf is None:
            sys.exit('rust_hand: no build statement for %s' % path)
        var = variables(nf, path)
        content = ('--crate-name %(crate_name)s --crate-type %(crate_type)s %(rustflags)s '
                   '%(rustdeps)s %(externs)s SOURCES %(sources)s') % {
            k: var.get(k, '') for k in ('crate_name', 'crate_type', 'rustflags', 'rustdeps',
                                        'externs', 'sources')}
        rsp = os.path.join(out_dir, var['target_out_dir'], var['label_name'] + '.rsp')
        os.makedirs(os.path.dirname(rsp), exist_ok=True)
        open(rsp, 'w').write(content)
    os.makedirs(os.path.dirname(os.path.join(out_dir, path)), exist_ok=True)
    cmd = command(path)
    if not cmd:
        sys.exit('rust_hand: no command for %s' % path)
    r = subprocess.run(['bash', '-c', cmd], cwd=out_dir, env=env)
    if r.returncode != 0:
        sys.exit('rust_hand: %s failed' % path)


for w in wanted:
    ensure(w)
