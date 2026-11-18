#!/bin/bash
#
# blink.sh -- compiling Blink for TessronOS, a stage at a time (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   blink.sh labels    the gn labels Blink's core stands on ($OBJ/labels_blink.txt)
#   blink.sh gen       their generated headers: the ones that depend on the
#                      target made here, the rest copied from $GENFROM
#                      (another renderer build of the same checkout)
#   blink.sh sample    a sample of the closure compiled (all of wtf, every
#                      40th source of the rest of Blink, every 10th of base)
#   blink.sh all       the whole closure compiled, $UNITY (8) sources of one
#                      target to a translation unit
#
# The labels leave out what tools/browser/exclude_blink.txt names. The
# objects go where translate.py puts them ($OBJ/$OUT); the log of a run is
# $OBJ/blink_<stage>.log.
#
# TS_ARCH=x86_64 does the same for the x86-64 build ($CR/out/tessronos_x64,
# ~/tessronos-obj-x64): the generated headers that are not made here are
# copied from the aarch64 build ($CR/out/tessronos/gen), the bindings and
# the mojom being the same for both.
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CR=${CR:-$HOME/cr6/src}
if [ "${TS_ARCH:-aarch64}" = x86_64 ]; then
	OUT=${OUT:-tessronos_x64}
	OBJ=${OBJ:-$HOME/tessronos-obj-x64}
	GENFROM=${GENFROM:-$CR/out/tessronos/gen}
fi
OUT=${OUT:-tessronos}
OBJ=${OBJ:-$HOME/tessronos-obj}
JOBS=${JOBS:-4}
GN=${GN:-$HOME/btron-upgrade/gnpkg153/gn}
PKGSTUB=${PKGSTUB:-$HOME/btron-upgrade/pkgstub}
PYBIN=${PYBIN:-$HOME/bin}
GENFROM=${GENFROM:-$CR/out/btron/gen}
ROOT=${ROOT:-//third_party/blink/renderer/core:core}
LABELS=$OBJ/labels_blink.txt

translate() {
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$LABELS" --exclude-list "$HERE/exclude_blink.txt" "$@"
}

case "${1:-}" in
labels)
	cd "$CR"
	PATH="$PYBIN:$PATH" PKG_CONFIG_PATH="$PKGSTUB" \
		"$GN" desc "out/$OUT" "$ROOT" deps --all | grep '^//' | grep -v '(//' > "$LABELS"
	echo "$ROOT" >> "$LABELS"
	echo "blink: $(wc -l < "$LABELS") labels"
	translate --list | grep -c '\.o$' | sed 's/^/blink: sources to compile /'
	;;
gen)
	grep -E 'buildflag|_flags$|:features$|build_config' "$LABELS" | sed 's#^//##' > "$OBJ/gen_blink.txt"
	# x86-64: optimization_guide's features reach DevTools' inspector overlay,
	# whose build needs esbuild, which the checkout does not have; its
	# generated headers are the aarch64 build's, copied below
	if [ "${TS_ARCH:-aarch64}" = x86_64 ]; then
		sed -i '/^components\/optimization_guide\/core:features$/d' "$OBJ/gen_blink.txt"
		# what the host's tools cannot be built for (the host's compiler is
		# clang over the system's older C++ library) is the aarch64 build's,
		# copied below
		GENLISTS="$OBJ/gen_blink.txt" JOBS="$JOBS" HOSTGEN_NINJA_FLAGS="-k 0" \
			bash "$HERE/hostgen.sh" || echo "blink: some generated sources are taken from $GENFROM"
	else
		GENLISTS="$OBJ/gen_blink.txt" JOBS="$JOBS" bash "$HERE/hostgen.sh"
	fi
	rsync -a --ignore-existing "$GENFROM/" "$CR/out/$OUT/gen/"
	# Blink speaks to Dawn through the client of its wire (design 14.6.5),
	# which the browser links, although the engine's graph is made without
	# Dawn (args.gn): what is under USE_DAWN is wanted
	sed -i 's/^#define BUILDFLAG_INTERNAL_USE_DAWN() (0)/#define BUILDFLAG_INTERNAL_USE_DAWN() (1)/' \
		"$CR/out/$OUT/gen/ui/gl/buildflags.h"
	;;
sample)
	translate --list | grep '\.o$' > "$OBJ/blink_all.objs"
	python3 - "$OBJ" <<'EOF'
import os, sys
obj = sys.argv[1]
objs = [l.strip() for l in open(os.path.join(obj, 'blink_all.objs'))]
pick = [o for o in objs if '/obj/third_party/blink/renderer/platform/wtf/' in o]
rest = [o for o in objs if '/obj/third_party/blink/' in o and o not in pick]
pick += rest[::40]
pick += [o for o in objs if '/obj/base/base/' in o][::10]
open(os.path.join(obj, 'blink_sample.txt'), 'w').write('\n'.join(pick) + '\n')
print('blink: sample of %d' % len(pick))
EOF
	translate --only "$OBJ/blink_sample.txt" 2>&1 | tee "$OBJ/blink_sample.log" | tail -20
	;;
all)
	translate --unity "${UNITY:-8}" --objlist "$OBJ/$OUT/blink.objs" 2>&1 | tee "$OBJ/blink_all.log" | tail -20
	;;
*)
	echo "blink: labels | gen | sample | all"
	exit 1
	;;
esac
