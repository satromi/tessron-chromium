#!/usr/bin/env python3
#
# blink_stubs.py -- stand-ins for what the Blink program names and the
# port does not build (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   blink_stubs.py NM CXXFILT PROBE.elf OUT.S [x86_64] [--allow FILE] [--refs OBJ...]
#
# PROBE.elf is the program linked with its unresolved symbols left at 0.
# For each symbol still undefined in it, OUT.S defines a stand-in: a
# function returns 0 (in x0, x1 and d0; on x86-64 in rax, rdx and xmm0,
# when the last argument says so) after br_stub_note has said, the
# first time, which one was called; a variable is 256 zeroed bytes (4 KB
# for a table of virtual functions). What stands behind a stand-in is a
# part of the engine the port leaves out (media, storage, devices, the
# other modules, the network stack): a page that reaches for it finds
# nothing there.
#
# A name of the port among them stops the run (see PORT_PREFIX); --refs
# names the port's objects, which are searched to say where it is named.
#

import re
import subprocess
import sys

# What the port itself defines: a name of it left undefined is a mistake
# (a definition hidden in an unnamed namespace, a file not built), never a
# part of the engine left out. A C name tells nothing of whether it is a
# function or a variable, so a variable of the port given a stand-in would
# be a function's few bytes in .text, written over at run time.
#
# The port's names are: the functions and variables of the port (br_),
# of its C++ runtime (ts_) and of the kernel and the system's libraries
# (knl_ tk_ wm_ ob_ so_ dp_ ui_ fs_ om_ gp_ ns_ usb_), found as a C name or
# as a C++ function of no namespace; and what stands in the port's own
# namespaces and classes (tessronos:: br:: brgl:: Br*). What stands in a
# namespace of the engine (blink:: gpu:: base:: net:: ...) is the engine's.
PORT_PREFIX = r'(br_|ts_|knl_|tessronos|tk_|wm_|ob_|so_|dp_|ui_|fs_|om_|gp_|ns_|usb_)'
PORT_NAME = re.compile(r'^' + PORT_PREFIX)
PORT_DEMANGLED = re.compile(r'(^|[ :(<,*&])(tessronos::|br::|brgl::|Br[A-Z])')
# a C++ function or variable of no namespace, after what c++filt puts
# before the name of a table of virtual functions and the like
PORT_GLOBAL = re.compile(r'^(?:(?:vtable|typeinfo|typeinfo name|guard variable|VTT|'
                         r'non-virtual thunk|virtual thunk) (?:for|to) )?' + PORT_PREFIX + r'\w*\(')

# A name the port leaves undefined on purpose: one per line, the name as
# nm prints it, then the reason after a '#'.
ALLOW_FILE = 'blink_stubs_allow.txt'


def is_port(sym, dem):
    return bool(PORT_NAME.match(sym) or PORT_DEMANGLED.search(dem) or PORT_GLOBAL.match(dem))


def read_allow(path):
    names = set()
    if path:
        with open(path) as f:
            for l in f:
                l = l.split('#', 1)[0].strip()
                if l:
                    names.add(l)
    return names


def explain(nm, cxxfilt, offenders, objs):
    # Which objects name each offender, and where one of them holds a
    # definition of its own that no other object can reach.
    refs, hidden = {}, {}
    want = set(s for s, _ in offenders)
    dem = dict(offenders)
    for i in range(0, len(objs), 200):
        part = objs[i:i + 200]
        r = subprocess.run([nm, '-A', '--no-demangle'] + part, capture_output=True, text=True)
        for l in r.stdout.splitlines():
            f = l.split()
            if len(f) < 3:
                continue
            where, kind, name = f[0].rsplit(':', 1)[0], f[-2], f[-1]
            if kind == 'U' and name in want:
                refs.setdefault(name, set()).add(where)
        r = subprocess.run([nm, '-A', '-C', '--defined-only'] + part, capture_output=True, text=True)
        for l in r.stdout.splitlines():
            f = l.split(None, 2)
            if len(f) < 3:
                continue
            for s in want:
                d = dem[s]
                if f[2].endswith('(anonymous namespace)::' + d) or (f[2] == d and f[1].islower()):
                    hidden.setdefault(s, set()).add(f[0].rsplit(':', 1)[0])
    for s, d in offenders:
        sys.stderr.write('  %s\n' % d)
        for w in sorted(refs.get(s, [])):
            sys.stderr.write('      named in %s\n' % w)
        for w in sorted(hidden.get(s, [])):
            sys.stderr.write('      a local definition of it in %s\n' % w)


def main():
    args = sys.argv[1:]
    nm, cxxfilt, probe, out = args[:4]
    rest = args[4:]
    x64 = bool(rest) and rest[0] == 'x86_64'
    if x64:
        rest = rest[1:]
    allow, objs = None, []
    while rest:
        a = rest.pop(0)
        if a == '--allow':
            allow = rest.pop(0)
        elif a == '--refs':
            objs, rest = rest, []
    allowed = read_allow(allow)
    r = subprocess.run([nm, '-u', probe], capture_output=True, text=True, check=True)
    syms = sorted(set(l.split()[-1] for l in r.stdout.splitlines()
                      if l.strip().startswith('U ')))
    dm = subprocess.run([cxxfilt], input='\n'.join(syms), capture_output=True,
                        text=True, check=True).stdout.splitlines()
    port = [(s, d) for s, d in zip(syms, dm) if s not in allowed and is_port(s, d)]
    if port:
        sys.stderr.write('blink_stubs: names of the port are left undefined, and the link would\n'
                         'go through with a stand-in that does nothing. Look first for a definition\n'
                         'inside an unnamed namespace (or static), a declaration that differs from\n'
                         'the definition, and a file that is not built. On purpose, add the name\n'
                         'to tools/browser/%s with a reason.\n' % ALLOW_FILE)
        explain(nm, cxxfilt, port, objs)
        sys.exit(1)
    funcs, datas = [], []
    for s, d in zip(syms, dm):
        if s.startswith(('_ZTV', '_ZTI', '_ZTS', '_ZGV', '_ZTT')):
            datas.append(s)
        elif s.startswith('_Z'):
            (funcs if '(' in d else datas).append(s)
        else:
            funcs.append(s)
    with open(out, 'w') as f:
        f.write('/* made by tools/browser/blink_stubs.py: %d functions, %d variables */\n'
                % (len(funcs), len(datas)))
        f.write('\t.section .rodata.br_stub_names,"a"\n')
        for i, s in enumerate(funcs):
            f.write('.Ln%d:\t.asciz "%s"\n' % (i, s))
        if x64:
            # the name's address in r11, which no call passes anything in
            f.write('\t.text\n\t.p2align 4\n')
            f.write('br_stub_hit:\n'
                    '\tpushq %rbp\n'
                    '\tmovq %rsp, %rbp\n'
                    '\tmovq %r11, %rdi\n'
                    '\tcall br_stub_note\n'
                    '\tpopq %rbp\n'
                    '\txorl %eax, %eax\n'
                    '\txorl %edx, %edx\n'
                    '\tpxor %xmm0, %xmm0\n'
                    '\tret\n')
            for i, s in enumerate(funcs):
                f.write('\t.p2align 4\n\t.globl %s\n\t.type %s, @function\n%s:\n'
                        '\tleaq .Ln%d(%%rip), %%r11\n\tjmp br_stub_hit\n'
                        % (s, s, s, i))
            f.write('\t.bss\n')
            for s in datas:
                n = 4096 if s.startswith('_ZTV') else 256
                f.write('\t.p2align 4\n\t.globl %s\n\t.type %s, @object\n\t.size %s, %d\n'
                        '%s:\n\t.zero %d\n' % (s, s, s, n, s, n))
            print('blink_stubs: %d functions, %d variables' % (len(funcs), len(datas)))
            return
        f.write('\t.text\n\t.p2align 2\n')
        f.write('br_stub_hit:\n'
                '\tstp x29, x30, [sp, -16]!\n'
                '\tmov x29, sp\n'
                '\tmov x0, x16\n'
                '\tbl br_stub_note\n'
                '\tldp x29, x30, [sp], 16\n'
                '\tmov x0, #0\n'
                '\tmov x1, #0\n'
                '\tfmov d0, xzr\n'
                '\tret\n')
        for i, s in enumerate(funcs):
            f.write('\t.p2align 2\n\t.globl %s\n\t.type %s, %%function\n%s:\n'
                    '\tadrp x16, .Ln%d\n\tadd x16, x16, :lo12:.Ln%d\n\tb br_stub_hit\n'
                    % (s, s, s, i, i))
        f.write('\t.bss\n')
        for s in datas:
            n = 4096 if s.startswith('_ZTV') else 256
            f.write('\t.p2align 4\n\t.globl %s\n\t.type %s, %%object\n\t.size %s, %d\n'
                    '%s:\n\t.zero %d\n' % (s, s, s, n, s, n))
    print('blink_stubs: %d functions, %d variables' % (len(funcs), len(datas)))


if __name__ == '__main__':
    main()
