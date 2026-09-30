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
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CR=${CR:-$HOME/cr6/src}
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
	GENLISTS="$OBJ/gen_blink.txt" JOBS="$JOBS" bash "$HERE/hostgen.sh"
	rsync -a --ignore-existing "$GENFROM/" "$CR/out/$OUT/gen/"
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
