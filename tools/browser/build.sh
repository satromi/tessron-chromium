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
#   build.sh v8app OUT.elf SRC.cc OBJ...
#                             a TessronOS program with V8 in it: SRC.cc compiled
#                             as V8's own sources are, linked with the program's
#                             objects OBJ (built by the TessronOS makefile)
#   build.sh skiaprog OUT.elf the Skia test program (tests/uprog/skiaprog.cc)
#   build.sh browser OUT.elf  the accessory (application/browser)
#   build.sh blink OUT.elf    the accessory with Blink (after blink.sh all)
#
# The Chromium checkout is $CR (~/cr6/src), its build description
# $CR/out/$OUT, the objects $OBJ/$OUT (~/tessronos-obj/tessronos). The C++
# runtime and the threads come from the TessronOS build (build_make,
# obj/lib/libcxxrt.a and obj/lib/libpthread.a), which make BROWSER=1 has
# made before it calls this. With make GL=1, GL_LIB names TessronOS's Mesa
# (libtsgl.a), and the Blink program gets WebGL on ANGLE over it.
#
# TS_ARCH=x86_64 builds the same for x86-64 (design 14.3): the build
# description $CR/out/tessronos_x64 (args_x64.gn), the objects in
# ~/tessronos-obj-x64, the x86_64-elf toolchain, and the NASM routines of
# the codecs (nasm_objs.py). make TARGET=_X64_ BROWSER=1 sets it.
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
# this repository (tessron-chromium) and the TessronOS tree it is built into
BR=$(cd "$HERE/../.." && pwd)
TS=${TESSRONOS:-$(cd "$BR/../TessronOS" && pwd)}
CR=${CR:-$HOME/cr6/src}
X64=
[ "${TS_ARCH:-aarch64}" = x86_64 ] && X64=1
if [ -n "$X64" ]; then
	OUT=${OUT:-tessronos_x64}
	OBJ=${OBJ:-$HOME/tessronos-obj-x64}
	XB=${TS_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	XP=$XB/x86_64-elf-
	MARCH=-march=x86-64-v2
	NASM=${NASM:-$HOME/.local/xPacks/nasm/bin/nasm}
	LABELS_V8=$HERE/labels_v8_x64.txt
	LABELS_SKIA=$HERE/labels_skia_x64.txt
	LDSCRIPT=$TS/etc/linker/x64/program_cxx.ld
else
	OUT=${OUT:-tessronos}
	OBJ=${OBJ:-$HOME/tessronos-obj}
	XB=${TS_TOOLCHAIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	XP=$XB/aarch64-none-elf-
	MARCH=-march=armv8.2-a
	LABELS_V8=$HERE/labels_v8.txt
	LABELS_SKIA=$HERE/labels_skia.txt
	LDSCRIPT=$TS/etc/linker/user/program_cxx.ld
fi
JOBS=${JOBS:-10}
# Blink's translation units take a gigabyte or more each: fewer side by side
BLINK_JOBS=${BLINK_JOBS:-4}
CC=${XP}gcc
CXX=${XP}g++
AR=${XP}ar
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
# x86-64: the port's headers of x86-64 (port/shim_x64) before the others
[ -n "$X64" ] && TSFLAGS="$MARCH -O2 -g0 -fno-stack-protector \
 -ffunction-sections -fdata-sections '-DCHK_TKERNEL_CONST=(1)' \
 -D_POSIX_TIMERS=1 -D_POSIX_MONOTONIC_CLOCK=200112L -D_POSIX_PRIORITY_SCHEDULING=1 \
 -I${SHIM}_x64 -I$SHIM -I$TS/include/tscxx -I$TS/include/tsposix -I$TS/include -I$TS/config"

compat() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_compat"
	for f in "$BR"/application/browser/port/compat/*.c; do
		o="$OBJ/$OUT/obj/tessronos_compat/$(basename "${f%.c}").o"
		if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
			eval "$CC $TSFLAGS -std=gnu11 -Wall -c $f -o $o"
		fi
	done
}

# dav1d's NEON routines (AVIF): assembly the translation of the compile
# commands does not reach, assembled here with the target's include
# directories; linked with the generated assembly
dav1d_asm() {
	[ -z "$X64" ] || return 0
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

# FFmpeg's NEON routines (the decoders of the media pipeline): assembly the
# translation of the compile commands does not reach, assembled here with
# the defines and include directories gn gives FFmpeg
ffmpeg_asm() {
	[ -z "$X64" ] || return 0
	local ninja="$CR/out/$OUT/obj/third_party/ffmpeg/ffmpeg_internal.ninja"
	[ -f "$ninja" ] || return 0
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	local defs incs
	defs=$(sed -n 's/^defines = //p' "$ninja" | tr ' ' '\n' | grep -v '_LIBCPP\|_GLIBCXX' | tr '\n' ' ')
	incs=$(sed -n 's/^include_dirs = //p' "$ninja")
	grep -h '^build .*: asm ' "$ninja" | sed 's/.*: asm \([^ |]*\).*/\1/' | while read -r s; do
		local f="$CR/out/$OUT/$s"
		local o="$OBJ/$OUT/obj/tessronos_gen/ffmpeg_$(basename "${s%.S}").o"
		if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then
			(cd "$CR/out/$OUT" && "$CC" -march=armv8.2-a -c $defs -DHAVE_VFP_ARGS=1 $incs "$s" -o "$o")
		fi
	done
}

# x86-64: the SIMD routines of dav1d, FFmpeg, libaom, libjpeg-turbo, libvpx
# and OpenH264, which gn assembles with NASM in actions of its own, as one
# archive (the link takes the members the C code calls). NASM_OBJS is what
# the link takes, empty on aarch64.
nasm_lib() {
	NASM_OBJS=
	[ -n "$X64" ] || return 0
	python3 "$HERE/nasm_objs.py" "$NASM" "$CR" "$OUT" "$OBJ/$OUT" "$OBJ/$OUT/nasm.objs" "$JOBS"
	archive libnasm "$OBJ/$OUT/nasm.objs"
	NASM_OBJS="$OBJ/$OUT/libnasm.a"
}

# The generated assembly: V8's embedded builtins and ICU's data
gen_asm() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	for s in gen/v8/embedded.S gen/third_party/icu/icudtl_dat.S; do
		o="$OBJ/$OUT/obj/tessronos_gen/$(basename "${s%.S}").o"
		if [ ! -f "$o" ] || [ "$CR/out/$OUT/$s" -nt "$o" ]; then
			"$CC" $MARCH -c "$CR/out/$OUT/$s" -o "$o"
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
	# x86_64-elf has no crti and crtn: the TessronOS build makes them (cxxrt.mk)
	if [ -n "$X64" ]; then
		crti=$LIBDIR/cxxrt/crti.o
		crtn=$LIBDIR/cxxrt/crtn.o
	fi
	"$CXX" $MARCH -nostdlib -nostartfiles -static \
		-T "$LDSCRIPT" -Wl,--build-id=none -Wl,--gc-sections \
		-Wl,-Map,"$out.map" $LINK_EXTRA -o "$out" "$crti" "$crtbegin" \
		-Wl,--start-group "$@" -Wl,--end-group \
		-Wl,--whole-archive "$LIBDIR/libcxxrt.a" "$LIBDIR/libpthread.a" -Wl,--no-whole-archive \
		-Wl,--start-group -lstdc++ -lm -lc -lgcc -Wl,--end-group "$crtend" "$crtn"
}

# The Rust code V8 reaches (Temporal's crates), as an archive the programs
# with V8 alone take it from, once the Rust parts are built (rust.sh)
v8_rust() {
	[ -f "$OBJ/$OUT/obj/libtessronos_rust.a" ] && echo "$OBJ/$OUT/obj/libtessronos_rust.a"
	true
}

# Skia and the port's own pieces of it as one archive, and the compat objects
skia_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$LABELS_SKIA" --extras-file "$HERE/skia_extras.txt" \
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

# ANGLE (labels_angle.txt: its GL_ and EGL_ entry points, the shader
# translator and the OpenGL ES backend) as one archive, the GLES2
# interface WebGL is given on it (port/webgl/gl_angle.cc) and WebGL's
# contexts in the GPU process (port/webgl/gl_cmdbuf.cc), Skia's raster
# on the GPU in the process of the pages (port/gpu/br_gpu.cc), the GPU
# process (port/gpu/br_gpu_main.cc) and a renderer's side of it
# (port/gpu/br_gpu_client.cc), when the program is linked with
# TessronOS's Mesa: GL_LIB names its library (make GL=1). ANGLE_OBJS is
# what the link takes, empty without it. The port's files are linked as
# objects, not from an archive: what calls them names them weakly.
angle_lib() {
	ANGLE_OBJS=
	[ -n "$GL_LIB" ] && [ -f "$GL_LIB" ] || return 0
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_angle.txt" --exclude-list "$HERE/exclude_angle.txt" \
		--objlist "$OBJ/$OUT/angle.objs"
	archive libangle "$OBJ/$OUT/angle.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extra "application/browser/port/webgl/gl_angle.cc=third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.cc" \
		--extra "application/browser/port/webgl/gl_cmdbuf.cc=third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.cc" \
		--extra "application/browser/port/gpu/br_gpu.cc=third_party/blink/renderer/modules/webgl/webgl_rendering_context_base.cc" \
		--extra "application/browser/port/gpu/br_gpu_main.cc=gpu/ipc/service/gpu_channel_manager.cc" \
		--extra "application/browser/port/gpu/br_gpu_client.cc=services/viz/public/cpp/gpu/context_provider_command_buffer.cc"
	P="$OBJ/$OUT/obj/tessronos_port"
	ANGLE_OBJS="$P/gl_angle.o $P/gl_cmdbuf.o $P/br_gpu.o $P/br_gpu_main.o $P/br_gpu_client.o $OBJ/$OUT/libangle.a $GL_LIB"
}

# The GPU process's service of the command buffer (labels_gpu.txt: the
# decoders, the shared images, the scheduler and the sync points; the
# sources built only with Dawn and ui/gl/init's platform part,
# port/gpu/gl_init_tessronos.cc, from gpu_extras.txt), as one archive, on
# ANGLE: when the program is linked with TessronOS's Mesa (make GL=1).
# GPUSVC_OBJS is what the link takes, empty without it.
gpusvc_lib() {
	GPUSVC_OBJS=
	[ -n "$GL_LIB" ] && [ -f "$GL_LIB" ] || return 0
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_gpu.txt" --exclude-list "$HERE/exclude_gpu.txt" \
		--objlist "$OBJ/$OUT/gpusvc.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extras-file "$HERE/gpu_extras.txt"
	grep -v '^#' "$HERE/gpu_extras.txt" | grep . | sed 's/=.*//; s|.*/||; s|\.cc$|.o|' |
		sed "s|^|$OBJ/$OUT/obj/tessronos_port/|" >> "$OBJ/$OUT/gpusvc.objs"
	archive libgpusvc "$OBJ/$OUT/gpusvc.objs"
	GPUSVC_OBJS="$OBJ/$OUT/libgpusvc.a"
}

# CLD3 and the protobuf runtime it reads its feature specification with
# (labels_ai.txt), as one archive: the LanguageDetector's model
# (application/browser/port/blink/ai_tessronos.cc)
ai_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_ai.txt" --objlist "$OBJ/$OUT/ai.objs"
	archive libai "$OBJ/$OUT/ai.objs"
}

# The LZMA SDK's XZ decoder (labels_xz.txt), as one archive: the data of
# the Widevine package the user installs is unpacked with it
# (application/browser/port/media/br_widevine.cc)
xz_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_xz.txt" --objlist "$OBJ/$OUT/xz.objs"
	archive libxz "$OBJ/$OUT/xz.objs"
}

# Chromium's WebNN service (labels_webnn.txt: its mojom, the graph builder,
# the context and the tensors), as one archive: navigator.ml, computed on
# the processor (application/browser/port/blink/webnn_tessronos.cc)
webnn_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_webnn.txt" --objlist "$OBJ/$OUT/webnn.objs"
	# the GPU's memory accounting the contexts and tensors keep: the GPU
	# process's service has it (gpusvc_lib, called before), and without
	# that service it is compiled here
	if [ -z "$GPUSVC_OBJS" ]; then
		python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' 		--extra "//gpu/command_buffer/service/memory_tracking.cc=services/webnn/webnn_context_impl.cc"
		echo "$OBJ/$OUT/obj/tessronos_port/memory_tracking.o" >> "$OBJ/$OUT/webnn.objs"
	fi
	archive libwebnn "$OBJ/$OUT/webnn.objs"
}

# The speech synthesizers and the recognizer, and the data they read
# (speech.sh), made again only when the script or the glue is newer than
# what it made: speechSynthesis and SpeechRecognition
# (port/blink/speech_tessronos.cc)
speech_lib() {
	disk_guard
	if [ ! -f "$OBJ/$OUT/libspeech.a" ] || [ ! -f "$OBJ/$OUT/obj/speech/speech.rec" ] \
	   || [ "$HERE/speech.sh" -nt "$OBJ/$OUT/libspeech.a" ] \
	   || [ "$BR/application/browser/port/speech/speech_glue.c" -nt "$OBJ/$OUT/libspeech.a" ]; then
		OBJ="$OBJ" OUT="$OUT" JOBS=4 bash "$HERE/speech.sh"
	fi
}

# PDFium (labels_pdfium.txt), as one archive: the PDF documents the page
# shows are drawn with it (application/browser/port/blink/pdf_tessronos.cc)
pdfium_lib() {
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_pdfium.txt" --objlist "$OBJ/$OUT/pdfium.objs"
	archive libpdfium "$OBJ/$OUT/pdfium.objs"
}

# Dawn (its native implementation with the OpenGL ES backend, its wire and
# its procs table) and Tint (labels_dawn.txt), from the gn output with Dawn
# on (gngen.sh DAWN=1), as one archive: Blink speaks to Dawn's wire as its
# client (the link takes only that without OpenGL ES). WebGPU's interface
# on them (port/webgpu/webgpu_tessronos.cc) is linked when the program is
# linked with TessronOS's Mesa (make GL=1), with WebGPU's interface of
# the command buffers of the GPU process (port/webgpu/webgpu_cmdbuf.cc)
# and the client's side of Dawn's wire on the command buffer (the wire
# client, its serializer and its memory transfer service), which gn puts
# in the engine's graph only with Dawn: they are compiled with the command
# of WebGPUImplementation, whose USE_DAWN parts call them (blink.sh turns
# USE_DAWN on). DAWN_OBJS is what the link takes.
dawn_lib() {
	DAWN_OBJS=
	if [ ! -f "$CR/out/${OUT}_dawn/compile_commands.json" ]; then
		echo "build: Dawn's build is not described (DAWN=1 tools/browser/gngen.sh)"
		exit 1
	fi
	disk_guard
	python3 "$HERE/translate.py" --cr "$CR" --out "${OUT}_dawn" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$HERE/labels_dawn.txt" --exclude-list "$HERE/exclude_dawn.txt" \
		--extra "//third_party/dawn/src/tint/utils/system/terminal_other.cc=third_party/dawn/src/tint/utils/system/env_other.cc" \
		--objlist "$OBJ/$OUT/dawn.objs"
	echo "$OBJ/${OUT}_dawn/obj/tessronos_port/terminal_other.o" >> "$OBJ/$OUT/dawn.objs"
	archive libdawn "$OBJ/$OUT/dawn.objs"
	DAWN_OBJS="$OBJ/$OUT/libdawn.a"
	[ -n "$GL_LIB" ] && [ -f "$GL_LIB" ] || return 0
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extra "application/browser/port/webgpu/webgpu_tessronos.cc=third_party/blink/renderer/modules/webgpu/gpu.cc" \
		--extra "application/browser/port/webgpu/webgpu_cmdbuf.cc=third_party/blink/renderer/modules/webgpu/gpu.cc" \
		--extra "//gpu/command_buffer/client/dawn_wire_client.cc=gpu/command_buffer/client/webgpu_implementation.cc" \
		--extra "//gpu/command_buffer/client/dawn_client_serializer.cc=gpu/command_buffer/client/webgpu_implementation.cc" \
		--extra "//gpu/command_buffer/client/dawn_client_memory_transfer_service.cc=gpu/command_buffer/client/webgpu_implementation.cc"
	local p="$OBJ/$OUT/obj/tessronos_port"
	DAWN_OBJS="$p/webgpu_tessronos.o $p/webgpu_cmdbuf.o $p/dawn_wire_client.o $p/dawn_client_serializer.o"
	DAWN_OBJS="$DAWN_OBJS $p/dawn_client_memory_transfer_service.o $OBJ/$OUT/libdawn.a"
}

# The interfaces between the browser and its renderers (design 14.13):
# application/browser/mojom/*.mojom made C++ by Mojo's generators of the
# checkout, into $OBJ/_brgen (the generated files are the same for both
# processors), again when a .mojom is newer than what was made of it
br_mojom() {
	local gen="$OBJ/_brgen" m rel
	local py="$CR/third_party/cpython3/host/bin/python3"
	[ -x "$py" ] || py=python3
	for m in "$BR"/application/browser/mojom/*.mojom; do
		rel=${m#$BR/}
		[ "$gen/$rel.cc" -nt "$m" ] && [ "$gen/$rel-shared.cc" -nt "$m" ] && continue
		mkdir -p "$gen"
		printf '{"c++": {}, "typescript": {}, "_metadata": {"module_typemaps": {"c++": [], "typescript": []}}}\n' \
			> "$gen/typemap.json"
		(
			cd "$CR/out/$OUT"
			"$py" ../../mojo/public/tools/mojom/mojom_parser.py --input-root "$BR" --output-root "$gen" \
				--mojoms "$m" --enable-feature is_posix --enable-feature is_linux
			for step in "--generate_non_variant_code --generate_message_ids" "--generate_non_variant_code" \
				    "--typemap $gen/typemap.json"; do
				"$py" ../../mojo/public/tools/bindings/mojom_bindings_generator.py --use_bundled_pylibs \
					-o "$gen" generate -d "$BR" -I "$BR" --bytecode_path gen/mojo/public/tools/bindings \
					$step -g c++ "$m"
			done
		)
		echo "build: $rel made C++"
	done
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

# The English dictionary of the spelling (tools/browser/spell, en_US):
# its affixes and its words linked into the program, which hunspell reads
# as its two files (port/spell/filemgr_tessronos.cc)
spell_dict() {
	mkdir -p "$OBJ/$OUT/obj/tessronos_gen"
	printf '\t.section .rodata\n\t.global br_spell_aff\n\t.global br_spell_aff_end\n\t.global br_spell_dic\n\t.global br_spell_dic_end\nbr_spell_aff:\n\t.incbin "%s"\nbr_spell_aff_end:\n\t.byte 0\nbr_spell_dic:\n\t.incbin "%s"\nbr_spell_dic_end:\n\t.byte 0\n' \
		"$HERE/spell/en_US.aff" "$HERE/spell/en_US.dic" > "$OBJ/$OUT/obj/tessronos_gen/spell_dict.S"
	"$CC" -c "$OBJ/$OUT/obj/tessronos_gen/spell_dict.S" -o "$OBJ/$OUT/obj/tessronos_spell_dict.o"
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
		--labels-file "$LABELS_V8" --extras-file "$HERE/v8_extras.txt" \
		--objlist "$OBJ/$OUT/v8.objs"
	;;
v8prog)
	out=${2:?output file}
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$LABELS_V8" --extras-file "$HERE/v8_extras.txt" \
		--objlist "$OBJ/$OUT/v8.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" \
		--filter '^$' --extra "tests/uprog/v8prog.cc=v8/samples/hello-world.cc"
	compat
	gen_asm
	archive libv8 "$OBJ/$OUT/v8.objs"
	link_program "$out" "$OBJ/$OUT/obj/tessronos_port/v8prog.o" "$OBJ/$OUT/libv8.a" \
		"$OBJ/$OUT"/obj/tessronos_gen/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o $(v8_rust)
	ls -la "$out"
	;;
v8app)
	out=${2:?output file}
	src=${3:?source}
	shift 3
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" \
		--labels-file "$LABELS_V8" --extras-file "$HERE/v8_extras.txt" \
		--objlist "$OBJ/$OUT/v8.objs"
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" \
		--filter '^$' --extra "$src=v8/samples/hello-world.cc"
	compat
	gen_asm
	archive libv8 "$OBJ/$OUT/v8.objs"
	link_program "$out" "$OBJ/$OUT/obj/tessronos_port/$(basename "${src%.cc}").o" "$@" "$OBJ/$OUT/libv8.a" \
		"$OBJ/$OUT"/obj/tessronos_gen/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o $(v8_rust)
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
		"$OBJ/$OUT"/obj/tessronos_txc/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o 		"$LIBDIR/libtsui.a"
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
	# the player of the media elements, with the flags of a source of modules;
	# its parts on the media library with the flags of the library's own
	M=third_party/blink/renderer/modules/storage/storage_area.cc
	# the browser's and the renderers' interfaces, made C++ first, with the
	# flags of a generated source of mojom
	br_mojom
	MJ=ui/base/dragdrop/mojom/drag_drop_types.mojom.cc
	MS=ui/base/dragdrop/mojom/drag_drop_types.mojom-shared.cc
	python3 "$HERE/translate.py" --cr "$CR" --out "$OUT" --obj "$OBJ" --jobs "$JOBS" --filter '^$' \
		--extra "application/browser/br_main_blink.cc=$R" --extra "application/browser/br_http.cc=$R" \
		--extra "application/browser/br_obj.cc=$R" --extra "application/browser/br_blink.cc=$R" \
		--extra "application/browser/br_loader.cc=$R" --extra "application/browser/br_bindings.cc=$B" \
		--extra "application/browser/br_proc.cc=$R" --extra "application/browser/br_rproc.cc=$R" \
		--extra "application/browser/br_uidata.cc=$R" \
		--extra "$OBJ/_brgen/application/browser/mojom/br_process.mojom.cc=$MJ" \
		--extra "$OBJ/_brgen/application/browser/mojom/br_process.mojom-shared.cc=$MS" \
		--extra "application/browser/br_tls.cc=$T" \
		--extra "application/browser/port/blink/storage_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/websocket_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/indexeddb_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/blob_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/worker_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/services_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/background_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/background_fetch_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/opfs_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/cache_storage_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/shared_worker_tessronos.cc=third_party/blink/renderer/core/exported/web_shared_worker_impl.cc" \
		--extra "application/browser/port/blink/files_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/service_worker_tessronos.cc=third_party/blink/renderer/modules/service_worker/service_worker_container.cc" \
		--extra "application/browser/port/blink/credentials_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/file_system_access_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/devices_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/serial_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/usb_hid_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/cookie_store_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/p2p_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//services/network/p2p/socket.cc=services/network/p2p/socket.cc" \
		--extra "//services/network/p2p/socket_tcp.cc=services/network/p2p/socket_tcp.cc" \
		--extra "//services/network/p2p/socket_udp.cc=services/network/p2p/socket_udp.cc" \
		--extra "//services/network/p2p/socket_throttler.cc=services/network/p2p/socket_throttler.cc" \
		--extra "//services/network/proxy_resolving_client_socket.cc=services/network/proxy_resolving_client_socket.cc" \
		--extra "//services/network/proxy_resolving_client_socket_factory.cc=services/network/proxy_resolving_client_socket_factory.cc" \
		--extra "//components/webrtc/fake_ssl_client_socket.cc=components/webrtc/fake_ssl_client_socket.cc" \
		--extra "application/browser/port/blink/web_transport_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/buckets_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/filesystem_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/pip_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/storage_access_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/pdf_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/ai_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/midi_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" 		--extra "application/browser/port/blink/direct_sockets_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/printing_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/blink/speech_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/speech/br_speech_rec.cc=$R" \
		--extra "application/browser/port/blink/webnn_tessronos.cc=third_party/blink/renderer/modules/ml/ml.cc" \
		--extra "application/browser/port/net/br_stream.cc=$R" --extra "application/browser/port/net/br_store.cc=$R" --extra "application/browser/br_settings.cc=$R" --extra "application/browser/port/net/br_data.cc=$R" \
		--extra "application/browser/port/net/br_tray.cc=$R" \
		--extra "application/browser/port/net/br_objfile.cc=$R" \
		--extra "application/browser/port/net/br_print.cc=$R" \
		--extra "application/browser/port/net/br_notify.cc=$R" \
		--extra "application/browser/port/net/br_cpu.cc=$R" \
		--extra "application/browser/port/net/br_sysnet.cc=$R" 		--extra "application/browser/port/net/br_usb.cc=$R" \
		--extra "application/browser/port/net/br_serial.cc=$R" \
		--extra "application/browser/port/blink/print_tessronos.cc=third_party/blink/renderer/core/page/print_context.cc" \
		--extra "application/browser/port/net/net_fetch_tessronos.cc=$N" \
		--extra "application/browser/port/net/net_service_tessronos.cc=$N" \
		--extra "application/browser/port/net/store_service_tessronos.cc=$N" \
		--extra "application/browser/port/net/br_cred_core.cc=$N" \
		--extra "application/browser/port/net/tcp_socket_tessronos.cc=$N" \
		--extra "application/browser/port/net/push_tessronos.cc=$N" \
		--extra "application/browser/port/net/udp_socket_tessronos.cc=$N" \
		--extra "application/browser/port/net/host_resolver_tessronos.cc=$N" \
		--extra "application/browser/port/net/net_platform_tessronos.cc=$N" \
		--extra "application/browser/port/net/root_store_tessronos.cc=$N" \
		--extra "application/browser/port/webgl/gl_tessronos.cc=$G" \
		--extra "application/browser/port/webgl/br_glsl.cc=$G" \
		--extra "application/browser/port/media/media_player.cc=$M" \
		--extra "application/browser/port/media/hls_tessronos.cc=$M" \
		--extra "application/browser/port/media/media_demuxer.cc=media/filters/ffmpeg_demuxer.cc" \
		--extra "application/browser/port/media/br_audio_sink.cc=media/renderers/audio_renderer_impl.cc" \
		--extra "application/browser/port/media/eme_tessronos.cc=third_party/blink/renderer/platform/media/web_encrypted_media_client_impl.cc" \
		--extra "application/browser/port/media/br_widevine.cc=$R" \
		--extra "application/browser/port/media/widevine_ks_tessronos.cc=media/cdm/cdm_adapter.cc" \
		--extra "//components/cdm/common/cdm_manifest.cc=media/cdm/cdm_adapter.cc" \
		--extra "//media/cdm/simple_cdm_allocator.cc=media/cdm/cdm_adapter.cc" \
		--extra "//media/cdm/simple_cdm_buffer.cc=media/cdm/cdm_adapter.cc" \
		--extra "application/browser/port/media/br_snd.cc=$R" \
		--extra "application/browser/port/media/br_mix.cc=$R" \
		--extra "application/browser/port/media/rtc_audio_tessronos.cc=third_party/blink/renderer/modules/webrtc/webrtc_audio_renderer.cc" \
		--extra "application/browser/port/media/br_cam.cc=$R" \
		--extra "application/browser/port/media/br_hevc.cc=$R" \
		--extra "application/browser/port/media/hevc_tessronos.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "//media/gpu/h265_decoder.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "//media/gpu/h265_dpb.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "//media/gpu/codec_picture.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "application/browser/port/blink/camera_tessronos.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "application/browser/port/blink/screen_tessronos.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "application/browser/port/media/br_screen.cc=$R" \
		--extra "application/browser/port/ext/br_ext_os.cc=$R" \
		--extra "application/browser/port/ext/br_ext.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "application/browser/port/ext/br_ext_frames.cc=third_party/blink/renderer/modules/webcodecs/video_frame.cc" \
		--extra "application/browser/port/spell/spell_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/spell/filemgr_tessronos.cc=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/affentry.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/affixmgr.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/csutil.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/hashmgr.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/hunspell.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/phonet.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/replist.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "//third_party/hunspell/src/hunspell/suggestmgr.cxx=third_party/blink/renderer/modules/storage/storage_area.cc" \
		--extra "application/browser/port/media/webcodecs_tessronos.cc=media/mojo/clients/mojo_video_encoder_metrics_provider.cc" \
		--extra "application/browser/port/media/web_audio_tessronos.cc=third_party/blink/renderer/platform/audio/audio_destination.cc"
	roots
	blink_pak
	spell_dict
	angle_lib
	gpusvc_lib
	dawn_lib
	dav1d_asm
	ffmpeg_asm
	nasm_lib
	pdfium_lib
	ai_lib
	webnn_lib
	xz_lib
	speech_lib
	# the Rust decoders of JPEG XL and AVIF, when they are built (rust.sh)
	RUST_OBJ=
	[ -f "$OBJ/$OUT/obj/tessronos_rust.o" ] && RUST_OBJ="$OBJ/$OUT/obj/tessronos_rust.o"
	P="$OBJ/$OUT/obj/tessronos_port"
	set -- "$P/br_main_blink.o" "$P/br_http.o" "$P/br_obj.o" "$P/br_blink.o" "$P/br_loader.o" \
		"$P/br_proc.o" "$P/br_rproc.o" "$P/br_uidata.o" "$P/br_process.mojom.o" "$P/br_process.mojom-shared.o" \
		"$P/br_bindings.o" "$P/br_tls.o" "$P/png_tessronos.o" "$P/SkPngCodec.o" "$P/SkPngEncoderImpl.o" \
		"$P/storage_tessronos.o" "$P/websocket_tessronos.o" "$P/indexeddb_tessronos.o" "$P/blob_tessronos.o" "$P/worker_tessronos.o" "$P/services_tessronos.o" "$P/background_tessronos.o" "$P/background_fetch_tessronos.o" "$P/opfs_tessronos.o" "$P/cache_storage_tessronos.o" "$P/shared_worker_tessronos.o" "$P/files_tessronos.o" "$P/service_worker_tessronos.o" "$P/credentials_tessronos.o" "$P/file_system_access_tessronos.o" "$P/devices_tessronos.o" "$P/serial_tessronos.o" "$P/usb_hid_tessronos.o" "$P/cookie_store_tessronos.o" "$P/p2p_tessronos.o" "$P/socket.o" "$P/socket_tcp.o" "$P/socket_udp.o" "$P/socket_throttler.o" "$P/proxy_resolving_client_socket.o" "$P/proxy_resolving_client_socket_factory.o" "$P/fake_ssl_client_socket.o" "$P/web_transport_tessronos.o" "$P/buckets_tessronos.o" "$P/filesystem_tessronos.o" "$P/pip_tessronos.o" "$P/storage_access_tessronos.o" "$P/pdf_tessronos.o" "$P/ai_tessronos.o" "$P/midi_tessronos.o" "$P/direct_sockets_tessronos.o" "$P/printing_tessronos.o" "$P/speech_tessronos.o" "$P/br_speech_rec.o" "$P/webnn_tessronos.o" "$P/br_serial.o" "$P/br_stream.o" "$P/br_store.o" "$P/br_settings.o" "$P/br_data.o" "$P/br_tray.o" "$P/br_objfile.o" "$P/br_print.o" "$P/br_notify.o" "$P/br_cpu.o" "$P/br_sysnet.o" "$P/br_usb.o" "$P/print_tessronos.o" "$P/net_fetch_tessronos.o" "$P/net_service_tessronos.o" "$P/store_service_tessronos.o" \
		"$P/br_cred_core.o" "$P/tcp_socket_tessronos.o" "$P/push_tessronos.o" "$P/udp_socket_tessronos.o" "$P/host_resolver_tessronos.o" "$P/net_platform_tessronos.o" \
		"$P/root_store_tessronos.o" "$P/gl_tessronos.o" "$P/br_glsl.o" \
		"$P/media_player.o" "$P/hls_tessronos.o" "$P/media_demuxer.o" "$P/br_audio_sink.o" "$P/eme_tessronos.o" \
		"$P/br_widevine.o" "$P/widevine_ks_tessronos.o" "$P/cdm_manifest.o" "$P/simple_cdm_allocator.o" "$P/simple_cdm_buffer.o" \
		"$P/br_snd.o" "$P/br_mix.o" "$P/rtc_audio_tessronos.o" "$P/br_cam.o" "$P/br_hevc.o" "$P/hevc_tessronos.o" "$P/h265_decoder.o" "$P/h265_dpb.o" "$P/codec_picture.o" "$P/camera_tessronos.o" "$P/screen_tessronos.o" "$P/br_screen.o" "$P/br_ext.o" "$P/br_ext_os.o" "$P/br_ext_frames.o" "$P/spell_tessronos.o" "$P/filemgr_tessronos.o" \
		"$P/affentry.o" "$P/affixmgr.o" "$P/csutil.o" "$P/hashmgr.o" "$P/hunspell.o" "$P/phonet.o" "$P/replist.o" "$P/suggestmgr.o" "$OBJ/$OUT/obj/tessronos_spell_dict.o" "$P/web_audio_tessronos.o" "$P/webcodecs_tessronos.o" $GPUSVC_OBJS $ANGLE_OBJS $DAWN_OBJS $NASM_OBJS \
		"$OBJ/$OUT/obj/tessronos_roots.o" $RUST_OBJ \
		"$OBJ/$OUT/obj/tessronos_blink_pak.o" "$OBJ/$OUT/libpdfium.a" "$OBJ/$OUT/libai.a" "$OBJ/$OUT/libwebnn.a" "$OBJ/$OUT/libxz.a" "$OBJ/$OUT/libspeech.a" "$OBJ/$OUT/libblink.a" \
		"$OBJ/$OUT"/obj/tessronos_compat/*.o "$OBJ/$OUT"/obj/tessronos_gen/*.o \
		"$LIBDIR/libtsui.a"
	# What is named and not built gets a stand-in (blink_stubs.py): the
	# program is linked once with those left at 0 to see which they are.
	echo "build: linking Blink ($(free -m | awk 'NR==2 { print $7 }') MB free)"
	# the engine's thread's waits on a condition and on a lock noted (br_main.cc)
	WAIT_WRAP="-Wl,--wrap=pthread_cond_wait -Wl,--wrap=pthread_cond_timedwait -Wl,--wrap=pthread_mutex_lock"
	# the console's lines put out by a thread of their own, not waited for (br_main.cc)
	WAIT_WRAP="$WAIT_WRAP -Wl,--wrap=_write"
	# and the C library's conversions of floating numbers run one at a time (port/compat/ts_libc.c)
	for f in _dtoa_r _ldtoa_r _strtod_r _strtod_l strtod strtod_l strtof strtof_l _strtodg_l; do
		WAIT_WRAP="$WAIT_WRAP -Wl,--wrap=$f"
	done
	# (a call to a symbol not there is far from the code once the text
	# passes 128 MB and cannot be put in place: the probe is written all
	# the same, for its undefined symbols are all it is read for)
	LINK_EXTRA="-Wl,--unresolved-symbols=ignore-all -Wl,--noinhibit-exec $WAIT_WRAP" \
		link_program "$OBJ/$OUT/probe.elf" "$@" || [ -f "$OBJ/$OUT/probe.elf" ]
	python3 "$HERE/blink_stubs.py" "${XP}nm" "${XP}c++filt" \
		"$OBJ/$OUT/probe.elf" "$OBJ/$OUT/obj/tessronos_gen/blink_stubs.s" ${X64:+x86_64} \
		--allow "$HERE/blink_stubs_allow.txt" --refs "$P"/*.o "$OBJ/$OUT"/obj/tessronos_compat/*.o
	"$CC" -c "$OBJ/$OUT/obj/tessronos_gen/blink_stubs.s" -o "$OBJ/$OUT/obj/tessronos_blink_stubs.o"
	rm -f "$OBJ/$OUT/probe.elf" "$OBJ/$OUT/probe.elf.map"
	# linked with its symbols kept on the Linux side ($OBJ/$OUT/<name>.sym,
	# and its map), and carried without them
	sym="$OBJ/$OUT/$(basename "$out").sym"
	LINK_EXTRA="$WAIT_WRAP" link_program "$sym" "$@" "$OBJ/$OUT/obj/tessronos_blink_stubs.o"
	"${XP}strip" -o "$out" "$sym"
	# the files of speech, record 2 of the program object (cxxrt.mk)
	cp "$OBJ/$OUT/obj/speech/speech.rec" "${out%.elf}_speech.rec"
	ls -la "$out"
	;;
*)
	echo "build: unknown stage $stage"
	exit 1
	;;
esac
