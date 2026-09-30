#!/bin/bash
#
# hostgen.sh -- the generated sources of the engine for aarch64 (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
# Runs, in $CR/out/$OUT (tools/browser/gngen.sh), what writes sources
# rather than objects for the target: V8's Torque output and bytecode
# lists, the snapshot and the embedded builtins made by mksnapshot (a host
# program with the arm64 simulator, built with Chromium's clang), and
# ICU's data as assembly. Extra ninja targets can be named as arguments.
#
# Stops before it starts when the Windows drive that holds WSL's disk has
# less than MINFREE_GB left: WSL's disk grows on it.
#
set -e
CR=${CR:-$HOME/cr6/src}
OUT=${OUT:-tessronos}
NINJA=${NINJA:-$HOME/btron-upgrade/gnpkg153/ninja}
PYBIN=${PYBIN:-$HOME/bin}
JOBS=${JOBS:-4}
MINFREE_GB=${MINFREE_GB:-8}

free_gb=$(df -BG /mnt/c | awk 'NR==2 { sub("G", "", $4); print $4 }')
if [ "${free_gb:-0}" -lt "$MINFREE_GB" ]; then
	echo "hostgen: only ${free_gb}GB left on C:, not starting"
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)
GENLISTS=${GENLISTS:-"$HERE/gen_v8.txt $HERE/gen_skia.txt"}
cd "$CR"
TARGETS="v8:run_mksnapshot_default v8:v8_generated_cc_files v8:run_torque \
	v8:generate_bytecode_builtins_list gen/third_party/icu/icudtl_dat.S $*"
for f in $GENLISTS; do
	[ -f "$f" ] && TARGETS="$TARGETS $(grep -v '^#' "$f" | tr '\n' ' ')"
done
#
# Some of what writes sources hangs off groups that also compile for the
# target; the target's compiler named in the build graph is not here, and
# those objects are made again by translate.py anyway. Stand-ins that only
# leave the output file let the graph run through.
FAKE=${FAKE:-$HOME/tessronos-obj/fakecc}
mkdir -p "$FAKE"
for t in gcc g++ ar nm readelf; do
	cat > "$FAKE/aarch64-linux-gnu-$t" <<'EOF'
#!/bin/sh
out=
while [ $# -gt 0 ]; do
	case "$1" in
	-o) out=$2; shift ;;
	-o*) out=${1#-o} ;;
	--output=*) out=${1#--output=} ;;
	esac
	shift
done
[ -n "$out" ] && : > "$out"
exit 0
EOF
	chmod +x "$FAKE/aarch64-linux-gnu-$t"
done
PATH="$PYBIN:$FAKE:$PATH" "$NINJA" -C "out/$OUT" -j "$JOBS" $TARGETS
ls -la "out/$OUT/gen/v8/embedded.S" "out/$OUT/gen/v8/snapshot.cc" \
	"out/$OUT/gen/third_party/icu/icudtl_dat.S"
