#
# browser.mk — ブラウザの移植(設計書 17.19)。make BROWSER=1 のときだけ読む
#
#   エンジン(Chromium の V8・Skia・Blink)はリポジトリに入れず、WSL のホームのチェックアウト
#   ($CR、既定 ~/cr6/src)から tools/browser/build.sh が TessronOS 向けに翻訳して結ぶ。
#   最初に一度 build.sh gngen と build.sh hostgen が要る(ビルドの記述と、生成するソース)。
#   翻訳した目的物は ~/tessronos-obj に置き、変わったものだけ翻訳し直す(結ぶのは毎回)。
#   BROWSER_PROGS で作るものを選ぶ(既定は全部)。
#

BROWSER_SH    := $(CHROMIUM)/tools/browser/build.sh
BROWSER_PROGS ?= v8prog skiaprog

.PHONY: browser-force
browser-force:

# 試験プログラム: tests/uprog/<名>.cc を obj/<名>.elf にし、/boot に <名>.ELF として載せる。
# ktest_v8 が走らせる
obj/v8prog.elf obj/skiaprog.elf: obj/%.elf: $(CXXRT_LIB) $(PTH_LIB) $(CHROMIUM)/tests/uprog/%.cc browser-force
	@mkdir -p $(dir $@)
	@echo 'ENG $@'
	@TESSRONOS=$(abspath $(TOP)) LIBDIR=$(abspath $(CXXRT_OUT)) bash $(BROWSER_SH) $* $(abspath $@)

EXTRA_BOOT_FILES += $(patsubst %,obj/%.elf,$(BROWSER_PROGS))

# 小物「ブラウザ」のプログラム(application/browser)。cxxrt.mk がレコード1にする。
# BROWSER_BLINK=1 なら頁を Blink で組む版(先に tools/browser/blink.sh all で Blink を翻訳しておく)。
# その実行形式は大きいので、試験用ディスクも大きくする
ifeq ($(BROWSER_BLINK),1)
BROWSER_STAGE := blink
TEST_DISK_MB  := 1024
SYS_VOL_SIZE  := 320M
else
BROWSER_STAGE := browser
endif

obj/browser.elf: $(CXXRT_LIB) $(PTH_LIB) $(wildcard $(CHROMIUM)/application/browser/*.cc) \
		 $(CHROMIUM)/application/browser/br.h browser-force
	@mkdir -p $(dir $@)
	@echo 'ENG $@'
	@TESSRONOS=$(abspath $(TOP)) LIBDIR=$(abspath $(CXXRT_OUT)) bash $(BROWSER_SH) $(BROWSER_STAGE) $(abspath $@)
