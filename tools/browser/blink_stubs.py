#!/usr/bin/env python3
#
# blink_stubs.py -- stand-ins for what the Blink program names and the
# port does not build (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   blink_stubs.py NM CXXFILT PROBE.elf OUT.S
#
# PROBE.elf is the program linked with its unresolved symbols left at 0.
# For each symbol still undefined in it, OUT.S defines a stand-in: a
# function returns 0 (in x0, x1 and d0) after br_stub_note has said, the
# first time, which one was called; a variable is 256 zeroed bytes (4 KB
# for a table of virtual functions). What stands behind a stand-in is a
# part of the engine the port leaves out (media, storage, devices, the
# other modules, the network stack): a page that reaches for it finds
# nothing there.
#

import subprocess
import sys


def main():
    nm, cxxfilt, probe, out = sys.argv[1:5]
    r = subprocess.run([nm, '-u', probe], capture_output=True, text=True, check=True)
    syms = sorted(set(l.split()[-1] for l in r.stdout.splitlines()
                      if l.strip().startswith('U ')))
    dm = subprocess.run([cxxfilt], input='\n'.join(syms), capture_output=True,
                        text=True, check=True).stdout.splitlines()
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
