#!/usr/bin/env python3
#
# gen_gl_forward.py -- the calls of WebGL's GLES2 interface given to ANGLE
# (design 14.6)
#
# This software is distributed under the MIT License (LICENSE).
#
#   gen_gl_forward.py [CR] [OUTDIR]
#
# writes OUTDIR/gl_angle_forward.inc (the overrides, inside the class) and
# OUTDIR/gl_angle_entry.inc (the declarations of the extensions' entry
# points it calls; ANGLE's header of them wants ANGLE's own gl2ext.h,
# which the engine's GLES2 headers stand in front of).
#
# Reads the methods of gpu::gles2::GLES2Interface (its autogen header) and
# the GLES entry points of ANGLE (libGLESv2/entry_points_gles_*_autogen.h,
# the GL_ names, which do not clash with Mesa's gl names), and writes one
# override per method for the class in gl_angle.cc: the context made
# current, then the entry point of the same name, timed (Timed). A method of a Chromium
# extension goes to ANGLE's entry point of the same function under another
# suffix (ANGLE, EXT, OES or none) when there is one with as many
# arguments. The methods gl_angle.cc implements itself (HAND) are left out;
# a method with no entry point notes the first call (Missing) and does
# nothing.

import os
import re
import sys

# Implemented in gl_angle.cc: shared images, synchronisation tokens, the
# extensions of the command buffer and what its client keeps on its side,
# the reads of pixels (the drawing buffer's frames are counted there), and
# the queries of the command buffer's own targets (fences and the
# completion of a link) with the link that such a query is around
HAND = {
    'CreateAndTexStorage2DSharedImageCHROMIUM',
    'CreateAndTexStorage2DSharedImageWithInternalFormatCHROMIUM',
    'BeginSharedImageAccessDirectCHROMIUM',
    'EndSharedImageAccessDirectCHROMIUM',
    'RequestExtensionCHROMIUM',
    'GetRequestableExtensionsCHROMIUM',
    'GetGraphicsResetStatusKHR',
    'ShallowFlushCHROMIUM',
    'OrderingBarrierCHROMIUM',
    'Flush',
    'Finish',
    'GetError',
    'GetString',
    'DeleteTextures',
    'GetBufferSubDataCHROMIUM',
    'ShallowFinishCHROMIUM',
    'GetMaxValueInBufferCHROMIUM',
    'ReadPixels',
    'BeginQueryEXT',
    'EndQueryEXT',
    'GetQueryObjectuivEXT',
    'DeleteQueriesEXT',
    'LinkProgram',
    # the core function in an OpenGL ES 3 context, the extension's in 2
    'DrawBuffersEXT',
    'BlitFramebufferCHROMIUM',
    'RenderbufferStorageMultisampleCHROMIUM',
}

# Extensions' entry points whose function is in the core of OpenGL ES 3.0:
# in an OpenGL ES 3 context (WebGL 2) the core one is called. ANGLE
# answers the extension's entry point with
# GL_INVALID_OPERATION when the extension is not on in the context, and
# some (ANGLE_instanced_arrays, EXT_draw_buffers) are not offered there.
# RenderbufferStorageMultisampleEXT is not here: it is
# EXT_multisampled_render_to_texture's, another function than the core's.
CORE_IN_ES3 = {
    'TexStorage2DEXT': 'TexStorage2D',
    'GenQueriesEXT': 'GenQueries',
    'IsQueryEXT': 'IsQuery',
    'GetQueryivEXT': 'GetQueryiv',
    'GenVertexArraysOES': 'GenVertexArrays',
    'DeleteVertexArraysOES': 'DeleteVertexArrays',
    'IsVertexArrayOES': 'IsVertexArray',
    'BindVertexArrayOES': 'BindVertexArray',
    'UnmapBufferOES': 'UnmapBuffer',
    'DrawArraysInstancedANGLE': 'DrawArraysInstanced',
    'DrawElementsInstancedANGLE': 'DrawElementsInstanced',
    'VertexAttribDivisorANGLE': 'VertexAttribDivisor',
}


def flat(text):
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'\s+', ' ', text)


def params(p):
    p = p.strip()
    if p in ('', 'void'):
        return []
    out = []
    depth = 0
    cur = ''
    for ch in p:
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch in '(<'
        depth -= ch in ')>'
        cur += ch
    out.append(cur.strip())
    return out


def pname(decl):
    m = re.search(r'(\w+)\s*(\[\s*\d*\s*\])?$', decl)
    return m.group(1)


def main():
    cr = sys.argv[1] if len(sys.argv) > 1 else os.path.expanduser('~/cr6/src')
    iface = flat(open(os.path.join(cr, 'gpu/command_buffer/client/gles2_interface_autogen.h')).read())
    methods = re.findall(r'virtual\s+([^;{}()]+?)\s*\b(\w+)\s*\(([^;]*?)\)\s*=\s*0\s*;', iface)

    angle = {}
    ext = {}
    decls = {}
    d = os.path.join(cr, 'third_party/angle/src/libGLESv2')
    for f in sorted(os.listdir(d)):
        m = re.match(r'entry_points_gles_(2_0|3_0|3_1|3_2|ext)_autogen\.h$', f)
        if not m:
            continue
        t = flat(open(os.path.join(d, f)).read())
        for ret, name, ps in re.findall(r'ANGLE_EXPORT\s+([^;]+?)\s*GL_APIENTRY\s+GL_(\w+)\s*\(([^;]*?)\)\s*;', t):
            angle[name] = len(params(ps))
            decls[name] = params(ps)
            if m.group(1) == 'ext':
                ext[name] = (ret, ps)

    out = []
    missing = []
    used = {'RequestExtensionANGLE', 'TexStorage2DEXT', 'BeginQueryEXT', 'EndQueryEXT',
            'GetQueryObjectuivEXT', 'DeleteQueriesEXT'}
    for ret, name, ps in methods:
        if name in HAND:
            continue
        args = params(ps)
        cands = [name]
        for suf in ('CHROMIUM', 'WEBGL', 'EXT', 'OES', 'KHR', 'ANGLE'):
            if name.endswith(suf):
                base = name[:-len(suf)]
                cands += [base + 'ANGLE', base + 'EXT', base + 'OES', base + 'KHR', base]
        target = next((c for c in cands if c in angle and angle[c] == len(args)), None)
        sig = '%s %s(%s) override' % (ret, name, ', '.join(args))
        if target is None:
            missing.append(name)
            body = 'Missing("%s");' % name
            if ret.strip() != 'void':
                body += ' return {};'
        else:
            used.add(target)
            # byte offsets into the element buffer, where ANGLE takes them
            # as pointers (the multi-draw calls): converted (Ptrs)
            names = []
            for a, t in zip(args, decls[target]):
                n = pname(a)
                if 'GLsizei*' in a.replace(' ', '') and 'void*const*' in t.replace(' ', ''):
                    n = 'Ptrs(%s, drawcount).data()' % n
                names.append(n)
            call = 'GL_%s(%s)' % (target, ', '.join(names))
            ret_ = '' if ret.strip() == 'void' else 'return '
            # what the call is, for the counts of the context's summary (gl_angle.cc, Timed)
            kind = ('kCallDraw' if name.startswith(('Draw', 'MultiDraw')) else
                    'kCallClear' if name.startswith('Clear') else 'kCallOther')
            timed = 'Timed t_(this, %s);' % kind
            core = CORE_IN_ES3.get(target)
            if core is not None and angle.get(core) == len(args):
                body = timed + ' if (es3_) { %sGL_%s(%s); } else { %s%s; }' % (
                    ret_, core, ', '.join(names), ret_, call)
            else:
                body = timed + ' %s%s;' % (ret_, call)
        out.append('%s { %s }' % (sig, body))

    outdir = sys.argv[2] if len(sys.argv) > 2 else '.'
    with open(os.path.join(outdir, 'gl_angle_forward.inc'), 'w') as f:
        f.write('// made by tools/browser/gen_gl_forward.py from gles2_interface_autogen.h and\n')
        f.write('// ANGLE\'s entry_points_gles_*_autogen.h: %d forwarded, %d without an entry point\n'
                % (len(out) - len(missing), len(missing)))
        for l in out:
            f.write(l + '\n')
    with open(os.path.join(outdir, 'gl_angle_entry.inc'), 'w') as f:
        f.write('// made by tools/browser/gen_gl_forward.py: the entry points of ANGLE\'s\n')
        f.write('// extensions that gl_angle.cc calls (entry_points_gles_ext_autogen.h)\n')
        f.write('extern "C" {\n')
        for name in sorted(used):
            if name in ext:
                ret, ps = ext[name]
                f.write('%s GL_APIENTRY GL_%s(%s);\n' % (ret.strip(), name, ps.strip()))
        f.write('}\n')
    print('gen_gl_forward: %d forwarded, %d without an entry point'
          % (len(out) - len(missing), len(missing)))


if __name__ == '__main__':
    main()
