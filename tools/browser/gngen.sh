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
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CR=${CR:-$HOME/cr6/src}
OUT=${OUT:-tessronos}
GN=${GN:-$HOME/btron-upgrade/gnpkg153/gn}
PKGSTUB=${PKGSTUB:-$HOME/btron-upgrade/pkgstub}
PYBIN=${PYBIN:-$HOME/bin}

[ -d "$CR" ] || { echo "gngen: no Chromium checkout at $CR"; exit 1; }
[ -x "$GN" ] || { echo "gngen: no gn at $GN"; exit 1; }
mkdir -p "$CR/out/$OUT"
cp "$HERE/args.gn" "$CR/out/$OUT/args.gn"
cd "$CR"
t0=$(date +%s)
PATH="$PYBIN:$PATH" PKG_CONFIG_PATH="$PKGSTUB" \
	"$GN" gen "out/$OUT" --export-compile-commands 2>&1 | tail -${TAIL:-40}
echo "gngen: $(( $(date +%s) - t0 ))s"
ls -la "out/$OUT/compile_commands.json"
