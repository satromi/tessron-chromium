#!/bin/bash
#
# speech.sh -- the speech synthesizers and recognizer of the browser
# (design 14.4.15), as one archive and the data they read
#
# This software is distributed under the MIT License (LICENSE).
#
# The sources are fetched once into $SPEECH/dl and unpacked into
# $SPEECH/src (they are not kept in the repository):
#
#   Open JTalk 1.11 and hts_engine API 1.10 (Japanese; modified BSD), the
#   dictionary naist-jdic for Open JTalk (BSD), the HTS voice of NIT
#   (nitech_jp_atr503_m001, CC BY 3.0);
#   Flite 2.2 with its voice cmu_us_slt (English; BSD-like);
#   whisper.cpp 1.7.6 and ggml (MIT) with the model ggml-tiny-q5_1 (MIT).
#
# They are compiled for the browser with the flags translate.py gives the
# engine (newlib, the port's POSIX and C++ headers), into
# $OBJ/$OUT/libspeech.a; the data (the dictionary, the voice, the model)
# is packed as one record, $OBJ/$OUT/obj/speech/speech.rec, which the
# build puts in the browser's program object as record 2 and the
# browser carries (port/blink/speech_tessronos.cc reads it).
#
# TS_ARCH=x86_64 compiles them for x86-64 (the x86_64-elf toolchain,
# ~/tessronos-obj-x64, ggml's x86 rows built for x86-64-v2).
#
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BR=$(cd "$HERE/../.." && pwd)
if [ "${TS_ARCH:-aarch64}" = x86_64 ]; then
	OBJ=${OBJ:-$HOME/tessronos-obj-x64}
	OUT=${OUT:-tessronos_x64}
	XB=${TS_TOOLCHAIN:-$HOME/.local/xPacks/x86_64-elf-gcc/bin}
	XP=$XB/x86_64-elf-
	GGML_ARCH=x86
else
	OBJ=${OBJ:-$HOME/tessronos-obj}
	OUT=${OUT:-tessronos}
	XB=${TS_TOOLCHAIN:-$HOME/.local/xPacks/aarch64-none-elf-gcc/bin}
	XP=$XB/aarch64-none-elf-
	GGML_ARCH=arm
fi
SPEECH=${SPEECH:-$HOME/speech}
JOBS=${JOBS:-4}
B=$OBJ/$OUT/obj/speech
mkdir -p "$SPEECH/dl" "$SPEECH/src" "$B"

fetch() {	# file url
	[ -s "$SPEECH/dl/$1" ] || curl -sSL -o "$SPEECH/dl/$1" "$2"
}
fetch hts_engine_API-1.10.tar.gz "https://downloads.sourceforge.net/project/hts-engine/hts_engine%20API/hts_engine_API-1.10/hts_engine_API-1.10.tar.gz"
fetch open_jtalk-1.11.tar.gz "https://downloads.sourceforge.net/project/open-jtalk/Open%20JTalk/open_jtalk-1.11/open_jtalk-1.11.tar.gz"
fetch open_jtalk_dic_utf_8-1.11.tar.gz "https://downloads.sourceforge.net/project/open-jtalk/Dictionary/open_jtalk_dic-1.11/open_jtalk_dic_utf_8-1.11.tar.gz"
fetch hts_voice_nitech_jp_atr503_m001-1.05.tar.gz "https://downloads.sourceforge.net/project/open-jtalk/HTS%20voice/hts_voice_nitech_jp_atr503_m001-1.05/hts_voice_nitech_jp_atr503_m001-1.05.tar.gz"
fetch flite-2.2.tar.gz "https://github.com/festvox/flite/archive/refs/tags/v2.2.tar.gz"
fetch whisper.cpp-1.7.6.tar.gz "https://github.com/ggml-org/whisper.cpp/archive/refs/tags/v1.7.6.tar.gz"
fetch ggml-tiny-q5_1.bin "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-tiny-q5_1.bin"
S=$SPEECH/src
for t in hts_engine_API-1.10 open_jtalk-1.11 open_jtalk_dic_utf_8-1.11 hts_voice_nitech_jp_atr503_m001-1.05 flite-2.2 whisper.cpp-1.7.6; do
	[ -d "$S/$t" ] || tar xzf "$SPEECH/dl/$t.tar.gz" -C "$S"
done

SHIM=$BR/application/browser/port/shim
INC=$OBJ/_inc
TF="-march=armv8.2-a -mtune=cortex-a76 -mno-outline-atomics -fno-stack-protector -ffunction-sections -fdata-sections
 -D__linux__ -D__linux -Dlinux -D__unix__ -D__unix -Dunix -D_GNU_SOURCE -D_POSIX_TIMERS=1
 -D_POSIX_MONOTONIC_CLOCK=200112L -D_POSIX_PRIORITY_SCHEDULING=1 -D_POSIX_CLOCK_SELECTION=200112L -D__TESSRONOS__=1 -I$SHIM -I$INC/tscxx -I$INC/tsposix -include $SHIM/ts_compat.h
 -O2 -w -DNDEBUG"
[ "$GGML_ARCH" = x86 ] && TF="-march=x86-64-v2 -mtune=generic -fno-stack-protector -ffunction-sections -fdata-sections
 -D__linux__ -D__linux -Dlinux -D__unix__ -D__unix -Dunix -D_GNU_SOURCE -D_POSIX_TIMERS=1
 -D_POSIX_MONOTONIC_CLOCK=200112L -D_POSIX_PRIORITY_SCHEDULING=1 -D_POSIX_CLOCK_SELECTION=200112L -D__TESSRONOS__=1 -I${SHIM}_x64 -I$SHIM -I$INC/tscxx -I$INC/tsposix -include $SHIM/ts_compat.h
 -O2 -w -DNDEBUG"
CC="${XP}gcc $TF -std=gnu11"
CXX="${XP}g++ $TF -std=gnu++17"

# The sources the files are read from in memory (speech_tessronos.cc
# gives them): mecab's dictionary and hts_engine's voice
P=$B/patched
rm -rf "$P"
mkdir -p "$P"
cp -r "$S/open_jtalk-1.11/mecab/src" "$P/mecab"
cp -r "$S/hts_engine_API-1.10/lib" "$P/hts"
cp -r "$S/hts_engine_API-1.10/include" "$P/hts_include"
cat > "$P/mecab/config.h" <<'EOF'
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define HAVE_SYS_STAT_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_DIRENT_H 1
#define MECAB_USE_UTF8_ONLY 1
EOF
python3 - "$P" <<'EOF'
import sys, re
p = sys.argv[1]
# mecab's Mmap (its POSIX half, the last): a file of the dictionary is the
# bytes the browser holds for its name, never freed
m = open(p + '/mecab/mmap.h', encoding='latin-1').read()
m = m.replace('#include "config.h"', '#include "config.h"\nextern "C" int br_speech_file(const char *name, const void **data, unsigned long *len);', 1)
a = m.rindex('  bool open(const char *filename, const char *mode = "r") {')
b = m.index('    return true;', a)
body = '''  bool open(const char *filename, const char *mode = "r") {
    this->close();
    const void *d = 0;
    unsigned long n = 0;
    if (!br_speech_file(filename, &d, &n)) {
      what_.stream_ << "no such dictionary file: " << filename;
      return false;
    }
    text = reinterpret_cast<T *>(const_cast<void *>(d));
    length = n;
    fileName = std::string(filename);
    flag = O_RDONLY;
    fd = -1;
'''
m = m[:a] + body + m[b:]
c = m.index('  void close() {', a)
e = m.index('  Mmap(): text(0), fd(-1) {}', c)
m = m[:c] + '  void close() {\n    text = 0;\n    fd = -1;\n  }\n\n' + m[e:]
open(p + '/mecab/mmap.h', 'w', encoding='latin-1').write(m)
# The settings file is not read (Open JTalk sets them in the code): its
# file stream, opened for nothing, is not made either. The library's
# file streams are built without threads and do not agree in size with
# what the headers, which have them, lay out (design 14.4.15).
q = open(p + '/mecab/param.cpp', encoding='latin-1').read()
q = q.replace('  std::ifstream ifs(WPATH(filename));\n', '', 1)
open(p + '/mecab/param.cpp', 'w', encoding='latin-1').write(q)
# hts_engine: a voice named "mem:<name>" is the bytes the browser holds
h = open(p + '/hts/HTS_misc.c', encoding='latin-1').read()
h = h.replace('HTS_File *HTS_fopen_from_fn(const char *name, const char *opt)\n{\n   HTS_File *fp = (HTS_File *) HTS_calloc(1, sizeof(HTS_File));\n',
 'extern int br_speech_file(const char *name, const void **data, unsigned long *len);\nHTS_File *HTS_fopen_from_fn(const char *name, const char *opt)\n{\n   const void *d;\n   unsigned long n;\n   if (strncmp(name, "mem:", 4) == 0 && br_speech_file(name + 4, &d, &n))\n      return HTS_fopen_from_data((void *) d, (size_t) n);\n   HTS_File *fp = (HTS_File *) HTS_calloc(1, sizeof(HTS_File));\n', 1)
h = h.replace('return (size_t) pos.__pos;', 'return (size_t) ftell((FILE *) fp->pointer);', 1)
open(p + '/hts/HTS_misc.c', 'w', encoding='latin-1').write(h)
EOF

objs=()
cc() {	# compiler src obj flags...
	local c=$1 src=$2 o=$3
	shift 3
	if [ ! -f "$o" ] || [ "$src" -nt "$o" ]; then
		$c "$@" -c "$src" -o "$o" || { echo "speech: $src failed"; exit 1; }
	fi
	objs+=("$o")
}

OJ=$S/open_jtalk-1.11
mkdir -p "$B/hts" "$B/ojt" "$B/mecab" "$B/flite" "$B/whisper"
for f in "$P"/hts/*.c; do
	cc "$CC" "$f" "$B/hts/$(basename "${f%.c}").o" -I"$P/hts_include" -DAUDIO_PLAY_NONE &
	[ $(jobs -r | wc -l) -ge $JOBS ] && wait -n
done
wait
for f in "$P"/hts/*.c; do objs+=("$B/hts/$(basename "${f%.c}").o"); done
OJI="-I$OJ/text2mecab -I$OJ/mecab/src -I$OJ/mecab2njd -I$OJ/njd -I$OJ/njd_set_pronunciation -I$OJ/njd_set_digit
 -I$OJ/njd_set_accent_phrase -I$OJ/njd_set_accent_type -I$OJ/njd_set_unvoiced_vowel -I$OJ/njd_set_long_vowel
 -I$OJ/njd2jpcommon -I$OJ/jpcommon -I$P/hts_include -DCHARSET_UTF_8 -DASCII_HEADER"
for d in text2mecab mecab2njd njd njd_set_pronunciation njd_set_digit njd_set_accent_phrase njd_set_accent_type \
	 njd_set_unvoiced_vowel njd_set_long_vowel njd2jpcommon jpcommon; do
	for f in "$OJ/$d"/*.c; do
		cc "$CC" "$f" "$B/ojt/$(basename "${f%.c}").o" $OJI
	done
done
MEC="-I$P/mecab -DHAVE_CONFIG_H -DDIC_VERSION=102 -DPACKAGE=\"open_jtalk\" -DVERSION=\"1.11\"
 -DMECAB_DEFAULT_RC=\"dummy\" -DMECAB_DEFAULT_CHARSET=\"UTF-8\" -DMECAB_USE_UTF8_ONLY -DCHARSET_UTF_8"
for f in char_property connector context_id dictionary dictionary_rewriter feature_index iconv_utils libmecab \
	 mecab nbest_generator param string_buffer tagger tokenizer utils viterbi writer; do
	cc "$CXX" "$P/mecab/$f.cpp" "$B/mecab/$f.o" $MEC $OJI -fexceptions
done

# Flite and its voice cmu_us_slt
FL=$S/flite-2.2
FLI="-I$FL/include -I$FL/lang/usenglish -I$FL/lang/cmulex -DCST_NO_SOCKETS -DCST_AUDIO_NONE"
for f in $(cd "$FL" && find src -name '*.c' ! -path '*/audio/au_*' ! -name 'cst_socket.c' ! -name 'cst_mmap_posix.c' ! -name 'cst_mmap_win32.c' \
		! -name 'cst_file_palmos.c' ! -name 'cst_file_wince.c' ! -path '*/wavesynth/*_lpc*' | sort) \
	 $(cd "$FL" && ls lang/usenglish/*.c lang/cmulex/*.c lang/cmu_us_slt/*.c | grep -Ev '(_raw|_huff_table|_num_bytes)[.]c$'); do
	o=$B/flite/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
	cc "$CC" "$FL/$f" "$o" $FLI -I"$FL/lang/cmu_us_slt"
done

# whisper.cpp and ggml, on the CPU (NEON; on x86-64 its SSE rows)
WH=$S/whisper.cpp-1.7.6
WHI="-I$WH/include -I$WH/ggml/include -I$WH/ggml/src -I$WH/ggml/src/ggml-cpu -DGGML_USE_CPU -DGGML_VERSION=\"0.0.0\" -DGGML_COMMIT=\"0\"
 -DGGML_SCHED_MAX_COPIES=4 -Dgetline=__getline"
for f in ggml/src/ggml.c ggml/src/ggml-alloc.c ggml/src/ggml-quants.c ggml/src/ggml-cpu/ggml-cpu.c \
	 ggml/src/ggml-cpu/quants.c ggml/src/ggml-cpu/arch/$GGML_ARCH/quants.c; do
	[ -f "$WH/$f" ] && cc "$CC" "$WH/$f" "$B/whisper/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')" $WHI
done
for f in src/whisper.cpp ggml/src/ggml.cpp ggml/src/ggml-backend.cpp ggml/src/ggml-backend-reg.cpp \
	 ggml/src/ggml-opt.cpp ggml/src/ggml-threading.cpp ggml/src/gguf.cpp ggml/src/ggml-cpu/ggml-cpu.cpp \
	 ggml/src/ggml-cpu/repack.cpp ggml/src/ggml-cpu/ops.cpp ggml/src/ggml-cpu/vec.cpp ggml/src/ggml-cpu/binary-ops.cpp \
	 ggml/src/ggml-cpu/unary-ops.cpp ggml/src/ggml-cpu/traits.cpp ggml/src/ggml-cpu/arch/$GGML_ARCH/repack.cpp; do
	[ -f "$WH/$f" ] && cc "$CXX" "$WH/$f" "$B/whisper/$(echo "$f" | tr '/' '_' | sed 's/\.cpp$/_cpp.o/')" $WHI -fexceptions
done

# the calls the browser makes (application/browser/port/speech/speech_glue.c)
mkdir -p "$B/glue"
cc "$CC" "$BR/application/browser/port/speech/speech_glue.c" "$B/glue/speech_glue.o" $OJI -I"$P/mecab" $FLI $WHI

rm -f "$OBJ/$OUT/libspeech.a"
"${XP}ar" rcs "$OBJ/$OUT/libspeech.a" "${objs[@]}"

# the data, compressed: the dictionary's files, the voice, the model
D=$B/data
mkdir -p "$D"
DIC=$S/open_jtalk_dic_utf_8-1.11
for f in sys.dic matrix.bin char.bin unk.dic; do
	[ "$D/$f.z" -nt "$DIC/$f" ] || python3 -c "import zlib,sys; open(sys.argv[2],'wb').write(zlib.compress(open(sys.argv[1],'rb').read(),9))" "$DIC/$f" "$D/$f.z"
done
[ -f "$D/voice.htsvoice" ] || cp "$S/hts_voice_nitech_jp_atr503_m001-1.05/nitech_jp_atr503_m001.htsvoice" "$D/voice.htsvoice"
[ -f "$D/whisper.bin" ] || cp "$SPEECH/dl/ggml-tiny-q5_1.bin" "$D/whisper.bin"
# one record: "BRSPCH01", the number of files (u32), for each a name of 32
# bytes, offset and size (u64), compressed (u32) and 4 bytes of nothing;
# the files after (port/speech/br_speech_rec.cc reads it)
python3 - "$D" "$OBJ/$OUT/obj/speech/speech.rec" <<'EOF'
import struct, sys
d, out = sys.argv[1], sys.argv[2]
files = [("sys.dic", "sys.dic.z", 1), ("matrix.bin", "matrix.bin.z", 1), ("char.bin", "char.bin.z", 1),
         ("unk.dic", "unk.dic.z", 1), ("voice.htsvoice", "voice.htsvoice", 0), ("whisper.bin", "whisper.bin", 0)]
blobs = [open(d + "/" + f, "rb").read() for _, f, _ in files]
off = 12 + 56 * len(files)
head = b"BRSPCH01" + struct.pack("<I", len(files))
for (name, _, z), b in zip(files, blobs):
    head += name.encode().ljust(32, b"\0") + struct.pack("<QQII", off, len(b), z, 0)
    off += len(b)
with open(out, "wb") as f:
    f.write(head)
    for b in blobs:
        f.write(b)
EOF
echo "speech: $(ls -l "$OBJ/$OUT/libspeech.a" | awk '{print $5}') bytes of code, $(ls -l "$OBJ/$OUT/obj/speech/speech.rec" | awk '{print $5}') of data"
