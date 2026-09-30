#!/bin/bash
#
# rust.sh -- the parts of the engine written in Rust, for TessronOS (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   rust.sh build    the Rust targets built by the checkout's own rules for
#                    aarch64 (rustc compiles the standard library too); the
#                    C and C++ steps among them are not the ones used, and a
#                    compiler that makes empty objects stands in for them
#   rust.sh merge    their rlibs merged into one relocatable object,
#                    $OBJ/$OUT/obj/tessronos_rust.o, linked with the engine
#
# The targets are the image decoders of JPEG XL (jxl-rs) and AVIF
# (CrabbyAvif, over dav1d and libyuv, which are C and are translated with
# the rest), the JSON parser base::JSONReader reads with (the network
# stack's certificate revocation list is JSON), Blink's XML parser (SVG,
# XHTML; built by hand, rust_edge.py), the check of a web font's format
# and Skia's reader of fonts (Fontations), and the allocator the Rust code
# allocates through. The C++ side
# of their bridges (the cxx code generated for them, the cxx runtime, the
# allocator's shim) is translated with the rest of the closure.
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CR=${CR:-$HOME/cr6/src}
OUT=${OUT:-tessronos}
OBJ=${OBJ:-$HOME/tessronos-obj}
NINJA=${NINJA:-$HOME/btron-upgrade/gnpkg153/ninja}
FAKECC=${FAKECC:-$OBJ/fakecc}
XB=${XB:-$HOME/.local/xPacks/xpack-aarch64-none-elf-gcc-15.2.1-1.1/bin}
TARGETS="third_party/rust/jxl/v0_5/wrapper:jxl_rs third_party/rust/jxl/v0_5/wrapper:jxl_rs_cxx_generated_gen third_party/crabbyavif:crabbyavif third_party/rust/serde_json_lenient/v0_2/wrapper:wrapper third_party/rust/xml/v1:lib third_party/blink/renderer/platform/fonts:font_format third_party/blink/renderer/platform/fonts:font_format_bindings_crubit_impl skia:bridge_rust_side build/rust/allocator:allocator"

case "${1:-}" in
build)
	cd "$CR"
	# inputs this checkout does not have (sources of targets that are not
	# built here, which the graph still names) stop ninja before it starts:
	# each is made an empty file for the run, and taken away after it
	made="$OBJ/rust_made_missing.txt"
	: > "$made"
	for i in $(seq 200); do
		miss=$(PATH="$FAKECC:$HOME/bin:$PATH" "$NINJA" -C "out/$OUT" -n $TARGETS 2>&1 |
			sed -n "s/^ninja: error: '\([^']*\)', needed by .*missing and no known rule to make it$/\1/p" | head -1)
		[ -n "$miss" ] || break
		mkdir -p "$(dirname "out/$OUT/$miss")"
		: > "out/$OUT/$miss"
		echo "$miss" >> "$made"
	done
	PATH="$FAKECC:$HOME/bin:$PATH" "$NINJA" -C "out/$OUT" -k 0 -j "${JOBS:-8}" $TARGETS \
		> "$OBJ/rust_ninja.log" 2>&1 || true
	(cd "out/$OUT" && while read -r f; do rm -f "$f"; done < "$made")
	rm -f "$made"
	# the steps of C and C++ fail or make nothing; the Rust ones must not
	if grep -A3 '^FAILED' "$OBJ/rust_ninja.log" | grep -q '^\[.*RUST\|rustc'; then
		echo "rust: a Rust step failed ($OBJ/rust_ninja.log)"
		exit 1
	fi
	# the crates ninja will not reach, built by hand (rust_edge.py): Blink's XML parser
	for edge in third_party/blink/renderer/core:xml_ffi; do
		d=${edge%%:*}
		n=${edge##*:}
		rlib=$(grep -F "build $(printf '%s' "$edge" | sed 's/:/$:/'): phony" "out/$OUT/build.ninja" |
			sed 's/^build [^:]*: phony //' | tr ' ' '\n' | grep '\.rlib$' | head -1)
		(cd "out/$OUT" && "$NINJA" -t commands -s "$rlib" | tail -1 > "$OBJ/rust_edge.cmd" &&
			PATH="$FAKECC:$PATH" python3 "$HERE/rust_edge.py" . "obj/$d/$n.ninja" "$OBJ/rust_edge.cmd") ||
			{ echo "rust: $edge failed"; exit 1; }
	done
	rm -f "$OBJ/rust_edge.cmd"
	echo "rust: $(find "out/$OUT" -name '*.rlib' -newer "out/$OUT/build.ninja" | wc -l) rlibs"
	;;
merge)
	B=$CR/out/$OUT
	W=$(mktemp -d)
	trap 'rm -rf "$W"' EXIT
	# every rlib for the target (the host's tools are x64_for_rust_host_build_tools);
	# test, getopts, unwind and profiler_builtins are not reached (panic=abort)
	find "$B/obj" "$B/local_rustc_sysroot/lib/rustlib/aarch64-unknown-linux-gnu" -name '*.rlib' |
		grep -v x64_for_rust_host |
		grep -vE '/lib(test_test|getopts_getopts|unwind_unwind|profiler_builtins_profiler_builtins)\.rlib$' |
		sort > "$W/rlibs.txt"
	n=0
	while read -r r; do
		for o in $("$XB/aarch64-none-elf-ar" t "$r" | grep '\.o$'); do
			"$XB/aarch64-none-elf-ar" p "$r" "$o" > "$W/$n-$(basename "$o")"
			n=$((n + 1))
		done
	done < "$W/rlibs.txt"
	echo "rust: $(wc -l < "$W/rlibs.txt") rlibs, $n objects"
	mkdir -p "$OBJ/$OUT/obj"
	"$XB/aarch64-none-elf-ld" -r -o "$OBJ/$OUT/obj/tessronos_rust.o" "$W"/*.o
	# compiler_builtins gives the C mathematics (floor, fma, sqrt, ...) as
	# weak symbols; kept inside the object, the C and C++ code keeps
	# newlib's
	"$XB/aarch64-none-elf-nm" "$OBJ/$OUT/obj/tessronos_rust.o" | awk '$2 == "W" { print $3 }' |
		grep -xE '(cbrt|ceil|copysign|fabs|fdim|floor|fma|fmax|fmaximum|fmaximum_num|fmin|fminimum|fminimum_num|fmod|rint|round|roundeven|sqrt|trunc)f?' \
		> "$W/libm.txt" || true
	"$XB/aarch64-none-elf-objcopy" --localize-symbols="$W/libm.txt" "$OBJ/$OUT/obj/tessronos_rust.o"
	ls -la "$OBJ/$OUT/obj/tessronos_rust.o"
	;;
*)
	echo "rust: build | merge"
	exit 1
	;;
esac
