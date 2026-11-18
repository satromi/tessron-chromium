#!/bin/bash
#
# gngen.sh -- the build description of the engine for TessronOS (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
# Makes $CR/out/$OUT with tools/browser/args.gn and has gn write the build
# graph and compile_commands.json for aarch64. Nothing is compiled here.
# The Chromium checkout ($CR) with its dependencies, the gn it was pinned
# to and the pkg-config descriptions standing in for host libraries the
# renderer never links are taken from where they already are.
#
#   CR=~/cr6/src GN=~/btron-upgrade/gnpkg153/gn \
#   PKGSTUB=~/btron-upgrade/pkgstub bash tools/browser/gngen.sh
#
# DAWN=1 makes $CR/out/tessronos_dawn instead, with the same arguments but
# Dawn on (its wire and its OpenGL ES backend): only Dawn's and Tint's
# sources are taken from it (design 14.6.5), so turning Dawn on does not
# change how the rest of the engine is built.
#
# TS_ARCH=x86_64 describes the x86-64 build instead (args_x64.gn), in
# $CR/out/tessronos_x64 and $CR/out/tessronos_x64_dawn.
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CR=${CR:-$HOME/cr6/src}
DAWN=${DAWN:-}
if [ "${TS_ARCH:-aarch64}" = x86_64 ]; then
	BASE=tessronos_x64
	ARGS=$HERE/args_x64.gn
else
	BASE=tessronos
	ARGS=$HERE/args.gn
fi
if [ -n "$DAWN" ]; then OUT=${OUT:-${BASE}_dawn}; else OUT=${OUT:-$BASE}; fi
GN=${GN:-$HOME/btron-upgrade/gnpkg153/gn}
PKGSTUB=${PKGSTUB:-$HOME/btron-upgrade/pkgstub}
PYBIN=${PYBIN:-$HOME/bin}

[ -d "$CR" ] || { echo "gngen: no Chromium checkout at $CR"; exit 1; }
[ -x "$GN" ] || { echo "gngen: no gn at $GN"; exit 1; }
mkdir -p "$CR/out/$OUT"
if [ -n "$DAWN" ]; then
	# Tint writes only GLSL for the backend (and WGSL for messages, and
	# nothing for what the backend only checks); Dawn's trace events go
	# nowhere; there is no X11
	{
		sed -e 's/^use_dawn = false/use_dawn = true/' \
		    -e 's/^dawn_enable_opengles = false/dawn_enable_opengles = true/' \
		    -e 's/^ozone_platform_x11 = true/ozone_platform_x11 = false\nozone_platform_headless = true/' \
		    -e 's/^use_x11 = true/use_x11 = false/' \
		    "$ARGS"
		printf '%s\n' 'tint_build_spv_reader = false' 'tint_build_spv_writer = false' \
			'tint_build_msl_writer = false' 'tint_build_hlsl_writer = false' \
			'tint_build_glsl_validator = false' \
			'tint_build_unittests = false' \
			'dawn_enable_perfetto = false'
	} > "$CR/out/$OUT/args.gn"
else
	cp "$ARGS" "$CR/out/$OUT/args.gn"
fi
cd "$CR"
t0=$(date +%s)
PATH="$PYBIN:$PATH" PKG_CONFIG_PATH="$PKGSTUB" \
	"$GN" gen "out/$OUT" --export-compile-commands 2>&1 | tail -${TAIL:-40}
echo "gngen: $(( $(date +%s) - t0 ))s"
if [ -n "$DAWN" ]; then
	# Dawn's sources written from its JSON and Tint's from their templates
	# (Tint's generator is in Go: Go is looked for where gclient would
	# have put it, tools/golang/<platform>), and the generated headers of
	# base and build the sources read, from the engine's own output
	NINJA=${NINJA:-$HOME/btron-upgrade/gnpkg153/ninja}
	T="third_party/dawn/src/tint:generate_sources"
	T="$T $(grep -E '_gen$|:gen$' "$HERE/labels_dawn.txt" | sed 's#^//##' | tr '\n' ' ')"
	PATH="$PYBIN:$PATH" "$NINJA" -C "out/$OUT" $T | tail -1
	for d in base build third_party/abseil-cpp; do
		[ -d "out/$BASE/gen/$d" ] || continue
		mkdir -p "out/$OUT/gen/$d"
		rsync -a --ignore-existing --include='*/' --include='*.h' --exclude='*' \
			"out/$BASE/gen/$d/" "out/$OUT/gen/$d/"
	done
fi
ls -la "out/$OUT/compile_commands.json"
