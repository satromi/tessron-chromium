#!/bin/bash
#
# build.sh -- the engine for TessronOS and the programs made of it (design 17.19)
#
# This software is distributed under the MIT License (LICENSE).
#
#   build.sh gngen            the build description for aarch64 (gngen.sh)
#   build.sh hostgen          the generated sources: Torque, snapshot, ICU data
#   build.sh v8 [JOBS]        V8 and what it needs, compiled for TessronOS
#   build.sh v8prog OUT.elf   the V8 test program (tests/uprog/v8prog.cc)
#   build.sh skiaprog OUT.elf the Skia test program (tests/uprog/skiaprog.cc)
#   build.sh browser OUT.elf  the accessory (application/browser)
#   build.sh blink OUT.elf    the accessory with Blink (after blink.sh all)
#
# The Chromium checkout is $CR (~/cr6/src), its build description
# $CR/out/$OUT, the objects $OBJ/$OUT (~/tessronos-obj/tessronos). The C++
# runtime and the threads come from the TessronOS build (build_make,
# obj/lib/libcxxrt.a and obj/lib/libpthread.a), which make BROWSER=1 has
# made before it calls this.
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
# this repository (tessron-chromium) and the TessronOS tree it is built into
BR=$(cd "$HERE/../.." && pwd)
TS=${TESSRONOS:-$(cd "$BR/../TessronOS" && pwd)}
CR=${CR:-$HOME/cr6/src}
OUT=${OUT:-tessronos}
OBJ=${OBJ:-$HOME/tessronos-obj}
JOBS=${JOBS:-10}
# Blink's translation units take a gigabyte or more each: fewer side by side
BLINK_JOBS=${BLINK_JOBS:-4}
XB=${TS_TOOLCHAIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
CC=$XB/aarch64-none-elf-gcc
CXX=$XB/aarch64-none-elf-g++
AR=$XB/aarch64-none-elf-ar
SHIM=$BR/application/browser/port/shim
LIBDIR=${LIBDIR:-$TS/build_make/obj/lib}
MINFREE_GB=${MINFREE_GB:-6}
export CR OUT OBJ

disk_guard() {
	local free_gb
	free_gb=$(df -BG /mnt/c 2>/dev/null | awk 'NR==2 { sub("G", "", $4); print $4 }')
	if [ -n "$free_gb" ] && [ "$free_gb" -lt "$MINFREE_GB" ]; then
		echo "build: only ${free_gb}GB left on C:, stopping"
		exit 1
	fi
}

# The flags of a TessronOS program's own C sources (build_make/cxxrt.mk)
TSFLAGS="-march=armv8.2-a -mtune=cortex-a76 -mno-outline-atomics -O2 -g0 -fno-stack-protector \
 -ffunction-sections -fdata-sections '-DCHK_TKERNEL_CONST=(1)' \
 -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L -D_POSIX_PRIORITY_SCHEDULING=1 \
 -I$SHIM -I$TS/include/tscxx -I$TS/include/tsposix -I$TS/include -I$TS/config"

compat() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_compat"
	for f in "$BR"/application/browser/port/compat/*.c; do
		o="$OBJ/$OUT/obj/tessronos_compat/$(basename "${f%.c}").o"
		if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
			eval "$CC $TSFLAGS -std=gnu11 -Wall -c $f -o $o"
		fi
	done
}

# The decoding of audio and video (port/media/media_decode.c) on FFmpeg
# (ffmpeg.sh); its objects, when FFmpeg is built, in $MEDIA_OBJS
FFMPEG=${FFMPEG:-$HOME/ffbt/ffmpeg-4.4.6}
media() {
	MEDIA_OBJS=
	local ff="$OBJ/ffmpeg"
	[ -f "$ff/lib/libavformat.a" ] || return 0
	local f="$BR/application/browser/port/media/media_decode.c"
	local o="$OBJ/$OUT/obj/tessronos_port/media_decode.o"
	mkdir -p "$OBJ/$OUT/obj/tessronos_port"
	if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
		eval "$CC $TSFLAGS -std=gnu11 -I$ff/include -I$FFMPEG -I$ff/build -c $f -o $o"
	fi
	MEDIA_OBJS="$o $ff/lib/libavformat.a $ff/lib/libavcodec.a $ff/lib/libswresample.a $ff/lib/libavutil.a"
}

# dav1d's NEON routines (AVIF): assembly the translation of the compile
# commands does not reach, assembled here with the target's include
# directories; linked with the generated assembly
dav1d_asm() {
	local d="$CR/third_party/dav1d"
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	grep -h '^build .*: asm ' "$CR/out/$OUT/obj/third_party/dav1d/dav1d_arm.ninja" |
		sed 's/.*: asm \([^ |]*\).*/\1/' | while read -r s; do
		local f="$CR/out/$OUT/$s"
		local o="$OBJ/$OUT/obj/tessronos_gen/dav1d_$(basename "${s%.S}").o"
		if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
			"$CC" -march=armv8.2-a -c -DPRIVATE_PREFIX=dav1d_ \
				-I"$d/libdav1d" -I"$d/libdav1d/include" -I"$d/config/linux/arm64" \
				-I"$d/libdav1d/src" "$f" -o "$o"
		fi
	done
}

# The generated assembly: V8's embedded builtins and ICU's data
gen_asm() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	for s in gen/v8/embedded.S gen/third_party/icu/icudtl_dat.S; do
		o="$OBJ/$OUT/obj/tessronos_gen/$(basename "${s%.S}").o"
		if [ ! -f "$o" ] || [ "$CR/out/$OUT/$s" -nt "$o" ]; then
			"$CC" -march=armv8.2-a -c "$CR/out/$OUT/$s" -o "$o"
		fi
	done
}

# One thin archive of every object a label list names, so that the link
# takes only the members something needs
archive() {
	local name=$1 list=$2
	rm -f "$OBJ/$OUT/$name.a"
	grep -v '\.fail$' "$list" | while read -r o; do [ -f "$o" ] && echo "$o"; done > "$list.ok"
	"$AR" rcsT "$OBJ/$OUT/$name.a" @"$list.ok"
}

# $(1): output, $(2)...: objects and archives, linked as a TessronOS C++ program
link_program() {
	local out=$1; shift
	local crti crtbegin crtend crtn
	crti=$($CC -print-file-name=crti.o)
	crtbegin=$($CC -print-file-name=crtbegin.o)
	crtend=$($CC -print-file-name=crtend.o)
	crtn=$($CC -print-file-name=crtn.o)
	"$CXX" -march=armv8.2-a -nostdlib -nostartfiles -static \
		-T "$TS/etc/linker/user/program_cxx.ld" -Wl,--build-id=none -Wl,--gc-sections \
		-Wl,-Map,"$out.map" $LINK_EXTRA -o "$out" "$crti" "$crtbegin" \
		-Wl,--start-group "$@" -Wl,--end-group \
		-Wl,--whole-archive "$LIBDIR/libcxxrt.a" "$LIBDIR/libpthread.a" -Wl,--no-whole-archive \
		-Wl,--start-group -lstdc++ -lm -lc -lgcc -Wl,--end-group "$crtend" "$crtn"
}

# Skia and the port's own pieces of it as one archive, and the compat objects
skia_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_skia.txt" --extras-file "$HERE/skia_extras.txt" \
		--exclude-list "$HERE/exclude_skia.txt" --objlist "$OBJ/$OUT/skia.objs"
	compat
	grep -h . "$OBJ/$OUT/skia.objs" > "$OBJ/$OUT/skiaall.objs"
	for x in SkFontMgr_custom_embedded SkMemory_malloc SkDiscardableMemory_none SkEventTracer skia_tessronos; do
		echo "$OBJ/$OUT/obj/tessronos_port/$x.o"
	done >> "$OBJ/$OUT/skiaall.objs"
	archive libskia "$OBJ/$OUT/skiaall.objs"
}

# Blink: the closure blink.sh all compiled ($OBJ/$OUT/blink.objs), the
# bindings of modules that core's interfaces are made with
# (blink_modules.txt), and the port's pieces of V8 and Skia, as one archive
blink_lib() {
	disk_guard
	if [ ! -f "$OBJ/$OUT/blink.objs" ]; then
		echo "build: Blink is not compiled (tools/browser/blink.sh all)"
		exit 1
	fi
	local filt
	filt="($(grep -v '^#' "$HERE/blink_modules.txt" | grep . | sed 's/\./\\./g' | paste -sd'|'))\$"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$BLINK_JOBS" \
		--filter "$filt" --objlist "$OBJ/$OUT/blinkmod.objs"
	# the sources of modules it is built with (blink_modules_src.txt)
	filt="/($(grep -v '^#' "$HERE/blink_modules_src.txt" | grep . | sed 's/\./\\./g' | paste -sd'|'))\$"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$BLINK_JOBS" \
		--filter "$filt" --objlist "$OBJ/$OUT/blinkmodsrc.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--filter '^$' --extras-file "$HERE/v8_extras.txt" \
		--extra "//third_party/skia/src/ports/SkFontMgr_custom_embedded.cpp=third_party/skia/src/ports/SkFontMgr_custom.cpp" \
		--extra "application/browser/port/base/allocator_tessronos.cc=base/process/memory.cc" \
		--extra "application/browser/port/base/base_tessronos.cc=base/debug/stack_trace.cc"
	# the image codecs in C in place of the Rust ones: libpng, and Skia's
	# codec on it under the Rust codec's names (linked as objects, before
	# the archive, whose Rust codec they stand for)
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_images.txt" --objlist "$OBJ/$OUT/images.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--filter '^$' --extras-file "$HERE/images_extras.txt"
	compat
	gen_asm
	{
		cat "$OBJ/$OUT/blink.objs" "$OBJ/$OUT/blinkmod.objs" "$OBJ/$OUT/blinkmodsrc.objs" \
			"$OBJ/$OUT/images.objs"
		echo "$OBJ/$OUT/obj/tessronos_port/platform-tessronos.o"
		echo "$OBJ/$OUT/obj/tessronos_port/SkFontMgr_custom_embedded.o"
		echo "$OBJ/$OUT/obj/tessronos_port/allocator_tessronos.o"
		echo "$OBJ/$OUT/obj/tessronos_port/base_tessronos.o"
	} > "$OBJ/$OUT/blinkall.objs"
	archive libblink "$OBJ/$OUT/blinkall.objs"
}

# Blink's resources (style sheets of the elements, scripts of controls) as data
blink_pak() {
	local pak="$CR/out/$OUT/gen/third_party/blink/public/resources/blink_resources.pak"
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	printf '\t.section .rodata\n\t.p2align 4\n\t.global br_blink_pak\n\t.global br_blink_pak_end\nbr_blink_pak:\n\t.incbin "%s"\nbr_blink_pak_end:\n\t.byte 0\n' \
		"$pak" > "$OBJ/$OUT/obj/tessronos_gen/blink_pak.S"
	"$CC" -c "$OBJ/$OUT/obj/tessronos_gen/blink_pak.S" -o "$OBJ/$OUT/obj/tessronos_blink_pak.o"
}

roots() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	awk '/-----BEGIN CERTIFICATE-----/ { p = 1 } p { print } /-----END CERTIFICATE-----/ { p = 0 }' \
		"$CR/net/data/ssl/chrome_root_store/root_store.certs" > "$OBJ/$OUT/obj/tessronos_gen/roots.pem"
	printf '\t.section .rodata\n\t.global br_roots\n\t.global br_roots_end\nbr_roots:\n\t.incbin "%s"\nbr_roots_end:\n\t.byte 0\n' \
		"$OBJ/$OUT/obj/tessronos_gen/roots.pem" > "$OBJ/$OUT/obj/tessronos_gen/roots.S"
	"$CC" -c "$OBJ/$OUT/obj/tessronos_gen/roots.S" -o "$OBJ/$OUT/obj/tessronos_roots.o"
}

stage=${1:-v8}

# One stage at a time: make runs the programs' rules side by side, and
# they share the archives and would each take gigabytes to link
mkdir -p "$OBJ/$OUT"
exec 9> "$OBJ/$OUT/.build.lock"
flock 9

case "$stage" in
gngen)
	bash "$HERE/gngen.sh"
	;;
hostgen)
	disk_guard
	bash "$HERE/hostgen.sh"
	;;
v8)
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_v8.txt" --extras-file "$HERE/v8_extras.txt" \
		--objlist "$OBJ/$OUT/v8.objs"
	;;
v8prog)
	out=${2:?output file}
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_v8.txt" --extras-file "$HERE/v8_extras.txt" \
		--objlist "$OBJ/$OUT/v8.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" \
		--filter '^$' --extra "tests/uprog/v8prog.cc=v8/samples/hello-world.cc"
	compat
	gen_asm
	archive libv8 "$OBJ/$OUT/v8.objs"
	link_program "$out" "$OBJ/$OUT/obj/tessronos_port/v8prog.o" "$OBJ/$OUT/libv8.a" \
		"$OBJ/$OUT"/obj/tessronos_gen/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o
	ls -la "$out"
	;;
skiaprog)
	out=${2:?output file}
	skia_lib
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" \
		--filter '^$' --extra "tests/uprog/skiaprog.cc=third_party/skia/src/ports/SkFontMgr_custom.cpp"
	link_program "$out" "$OBJ/$OUT/obj/tessronos_port/skiaprog.o" "$OBJ/$OUT/libskia.a" \
		"$OBJ/$OUT"/obj/tessronos_compat/*.o
	ls -la "$out"
	;;
browser)
	# the accessory: Skia (as skiaprog) and the text engine
	out=${2:?output file}
	skia_lib
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_tls.txt" --objlist "$OBJ/$OUT/tls.objs"
	archive libtls "$OBJ/$OUT/tls.objs"
	R=third_party/skia/src/ports/SkFontMgr_custom.cpp
	T=third_party/boringssl/src/ssl/ssl_lib.cc
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extra "application/browser/br_main.cc=$R" --extra "application/browser/br_http.cc=$R" \
		--extra "application/browser/br_text.cc=$R" --extra "application/browser/br_obj.cc=$R" \
		--extra "application/browser/br_tls.cc=$T"
	mkdir -p "$OBJ/$OUT/obj/tessronos_txc"
	for f in txc txc_tab; do
		eval "$CC $TSFLAGS -std=gnu11 -c $TS/lib/libtxc/$f.c -o $OBJ/$OUT/obj/tessronos_txc/$f.o"
	done
	# the root certificates: the PEM blocks of the engine's root store
	roots
	P="$OBJ/$OUT/obj/tessronos_port"
	link_program "$out" "$P/br_main.o" "$P/br_http.o" "$P/br_text.o" "$P/br_obj.o" "$P/br_tls.o" \
		"$OBJ/$OUT/obj/tessronos_roots.o" \
		"$OBJ/$OUT/libskia.a" "$OBJ/$OUT/libtls.a" \
		"$OBJ/$OUT"/obj/tessronos_txc/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o
	ls -la "$out"
	;;
blink)
	# the accessory with Blink as its engine (br_main_blink.cc)
	out=${2:?output file}
	blink_lib
	R=third_party/blink/renderer/core/loader/empty_clients.cc
	B=bindings/modules/v8/v8_window.cc
	T=third_party/boringssl/src/ssl/ssl_lib.cc
	# the port's network layer, with the flags of the network stack
	N=net/url_request/url_request.cc
	# and WebGL's GLES2 on the CPU, with the flags of WebGL's context
	G=third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.cc
	# the player of the media elements, with the flags of a source of modules
	M=third_party/blink/renderer/modules/storage/storage_area.cc
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extra "application/browser/br_main_blink.cc=$R" --extra "application/browser/br_http.cc=$R" \
		--extra "application/browser/br_obj.cc=$R" --extra "application/browser/br_blink.cc=$R" \
		--extra "application/browser/br_loader.cc=$R" --extra "application/browser/br_bindings.cc=$B" \
		--extra "application/browser/br_tls.cc=$T" \
		--extra "application/browser/port/blink/storage_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/net/br_stream.cc=$R" --extra "application/browser/port/net/br_store.cc=$R" \
		--extra "application/browser/port/net/net_fetch_tessronos.cc=$N" \
		--extra "application/browser/port/net/tcp_socket_tessronos.cc=$N" \
		--extra "application/browser/port/net/host_resolver_tessronos.cc=$N" \
		--extra "application/browser/port/net/net_platform_tessronos.cc=$N" \
		--extra "application/browser/port/net/root_store_tessronos.cc=$N" \
		--extra "application/browser/port/webgl/gl_tessronos.cc=$G" \
		--extra "application/browser/port/webgl/br_glsl.cc=$G" \
		--extra "application/browser/port/media/media_player.cc=$M" \
		--extra "application/browser/port/media/br_snd.cc=$R"
	roots
	blink_pak
	media
	dav1d_asm
	# the Rust decoders of JPEG XL and AVIF, when they are built (rust.sh)
	RUST_OBJ=
	[ -f "$OBJ/$OUT/obj/tessronos_rust.o" ] && RUST_OBJ="$OBJ/$OUT/obj/tessronos_rust.o"
	P="$OBJ/$OUT/obj/tessronos_port"
	set -- "$P/br_main_blink.o" "$P/br_http.o" "$P/br_obj.o" "$P/br_blink.o" "$P/br_loader.o" \
		"$P/br_bindings.o" "$P/br_tls.o" "$P/png_tessronos.o" "$P/SkPngCodec.o" "$P/SkPngEncoderImpl.o" \
		"$P/storage_tessronos.o" "$P/br_stream.o" "$P/br_store.o" "$P/net_fetch_tessronos.o" \
		"$P/tcp_socket_tessronos.o" "$P/host_resolver_tessronos.o" "$P/net_platform_tessronos.o" \
		"$P/root_store_tessronos.o" "$P/gl_tessronos.o" "$P/br_glsl.o" \
		"$P/media_player.o" "$P/br_snd.o" $MEDIA_OBJS \
		"$OBJ/$OUT/obj/tessronos_roots.o" $RUST_OBJ \
		"$OBJ/$OUT/obj/tessronos_blink_pak.o" "$OBJ/$OUT/libblink.a" \
		"$OBJ/$OUT"/obj/tessronos_compat/*.o "$OBJ/$OUT"/obj/tessronos_gen/*.o
	# What is named and not built gets a stand-in (blink_stubs.py): the
	# program is linked once with those left at 0 to see which they are.
	echo "build: linking Blink ($(free -m | awk 'NR==2 { print $7 }') MB free)"
	LINK_EXTRA="-Wl,--unresolved-symbols=ignore-all" link_program "$OBJ/$OUT/probe.elf" "$@"
	python3 "$HERE/blink_stubs.py" "$XB/aarch64-none-elf-nm" "$XB/aarch64-none-elf-c++filt" \
		"$OBJ/$OUT/probe.elf" "$OBJ/$OUT/obj/tessronos_gen/blink_stubs.s"
	"$CC" -c "$OBJ/$OUT/obj/tessronos_gen/blink_stubs.s" -o "$OBJ/$OUT/obj/tessronos_blink_stubs.o"
	rm -f "$OBJ/$OUT/probe.elf" "$OBJ/$OUT/probe.elf.map"
	# linked with its symbols kept on the Linux side ($OBJ/$OUT/<name>.sym,
	# and its map), and carried without them
	sym="$OBJ/$OUT/$(basename "$out").sym"
	link_program "$sym" "$@" "$OBJ/$OUT/obj/tessronos_blink_stubs.o"
	"$XB/aarch64-none-elf-strip" -o "$out" "$sym"
	ls -la "$out"
	;;
*)
	echo "build: unknown stage $stage"
	exit 1
	;;
esac
