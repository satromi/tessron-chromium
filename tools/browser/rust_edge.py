#!/usr/bin/env python3
#
# rust_edge.py -- one Rust crate of the checkout built by hand (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   rust_edge.py OUT-DIR TARGET.ninja COMMAND-FILE
#
# Some crates the engine calls (Blink's XML parser, xml_ffi) sit behind
# order-only inputs that reach sources this checkout does not have, so
# ninja will not build them. This writes the response file ninja would
# write for the rust_rlib edge (the rule's rspfile_content, from the
# target's .ninja variables) and runs the edge's command, which
# `ninja -t commands -s <rlib> | tail -1` gives.

import os
import re
import subprocess
import sys

out_dir, ninja_file, cmd_file = sys.argv[1:4]


def unescape(v):
    return re.sub(r'\$(.)', lambda m: m.group(1), v)


var = {}
for line in open(os.path.join(out_dir, ninja_file)):
    # the file's variables and those of the rlib's build statement alike
    m = re.match(r'^\s*([a-z_]+) = (.*)$', line.rstrip('\n'))
    if m:
        var[m.group(1)] = unescape(m.group(2))
content = ('--crate-name %(crate_name)s --crate-type %(crate_type)s %(rustflags)s '
           '%(rustdeps)s %(externs)s SOURCES %(sources)s') % {
    k: var.get(k, '') for k in ('crate_name', 'crate_type', 'rustflags', 'rustdeps',
                                'externs', 'sources')}
rsp = os.path.join(out_dir, var['target_out_dir'], var['label_name'] + '.rsp')
open(rsp, 'w').write(content)
cmd = open(cmd_file).read().strip()
r = subprocess.run(['bash', '-c', cmd], cwd=out_dir)
sys.exit(r.returncode)
