#
# browser.mk — ブラウザの移植(設計書 17.19)。make BROWSER=1 のときだけ読む
#
#   エンジン(Chromium の V8・Skia・Blink)はリポジトリに入れず、WSL のホームのチェックアウト
#   ($CR、既定 ~/cr6/src)から tools/browser/build.sh が TessronOS 向けに翻訳して結ぶ。
#   最初に一度 build.sh gngen と build.sh hostgen が要る(ビルドの記述と、生成するソース)。
#   翻訳した目的物は ~/tessronos-obj に置き、変わったものだけ翻訳し直す(結ぶのは毎回)。
#   BROWSER_PROGS で作るものを選ぶ(既定は全部)。
#   TARGET=_X64_ では x86-64 のものを作る(TS_ARCH=x86_64。目的物は ~/tessronos-obj-x64、
#   生成物は obj/x64。cxxrt.mk の BROWSER_OUT と BROWSER_ARCH_ENV、設計書 14.3)。
#

BROWSER_SH    := $(CHROMIUM)/tools/browser/build.sh
BROWSER_OUT   ?= obj
BROWSER_PROGS ?= v8prog skiaprog

.PHONY: browser-force
browser-force:

# 試験プログラム: tests/uprog/<名>.cc を obj/<名>.elf にし、/boot に <名>.ELF として載せる。
# ktest_v8 が走らせる
$(BROWSER_OUT)/v8prog.elf $(BROWSER_OUT)/skiaprog.elf: $(BROWSER_OUT)/%.elf: $(CXXRT_LIB) $(PTH_LIB) $(CHROMIUM)/tests/uprog/%.cc browser-force
	@mkdir -p $(dir $@)
	@echo 'ENG $@'
	@TESSRONOS=$(abspath $(TOP)) LIBDIR=$(abspath $(CXXRT_OUT)) $(BROWSER_ARCH_ENV) bash $(BROWSER_SH) $* $(abspath $@)

EXTRA_BOOT_FILES += $(patsubst %,$(BROWSER_OUT)/%.elf,$(BROWSER_PROGS))

# 試験 test_blink_wv だけのために、開発の機械で Raspberry Pi の倉庫から取ってきた索引(Packages)と
# Widevine のパッケージ(.deb)を、試験のディスクの /boot に wv_Packages・wv.deb として載せる。
# x86-64 では、Google のコンポーネントの更新のサービスの答え(アドレスを試験のサーバのものにしたもの)と
# Widevine CDM のコンポーネントの CRX3 を、wv_update.json・wv.crx3 として載せる。
# WV_TEST=<その二つのあるディレクトリ> を与えたときだけ。リポジトリにもシステムのボリュームにも入れない
ifneq ($(WV_TEST),)
ifeq ($(ARCH),x86_64)
EXTRA_BOOT_FILES += $(WV_TEST)/wv_update.json $(WV_TEST)/wv.crx3
else
EXTRA_BOOT_FILES += $(WV_TEST)/wv_Packages $(WV_TEST)/wv.deb
endif
endif

# 試験 test_blink_ubol だけのために、開発の機械で取ってきた実在の拡張機能(ZIP)を、試験のディスクの /boot に
# extest.zip として載せる。EXT_TEST=<そのファイル> を与えたときだけ。リポジトリにもシステムのボリュームにも入れない
ifneq ($(EXT_TEST),)
EXTRA_BOOT_FILES += $(EXT_TEST)
endif

# 小物「ブラウザ」のプログラム(application/browser)。cxxrt.mk がレコード1にする。
# BROWSER_BLINK=1 なら頁を Blink で組む版(先に tools/browser/blink.sh all で Blink を翻訳しておく)。
# その実行形式は大きい(音声の辞書とモデル、GL=1 では Mesa・ANGLE・Dawn を含む)ので、試験用ディスクとシステムのボリュームも大きくする。
# システムのボリュームは指定が無いときだけ 512M にする(Raspberry Pi のカードの像は makefile が先に 2G と決める)
ifeq ($(BROWSER_BLINK),1)
BROWSER_STAGE := blink
TEST_DISK_MB  := 1024
# x86-64 のディスクのイメージでは試験のプログラムが区画1(ESP)に載り、x86-64 のブラウザ(約225MB)と
# x86-64 のパッケージの分だけ、区画1とシステムのボリュームを大きくする
ifeq ($(ARCH),x86_64)
SD_BOOT_MB    ?= 1024
SYS_VOL_SIZE  ?= 768M
endif
SYS_VOL_SIZE  ?= 512M
else
BROWSER_STAGE := browser
endif

# make GL=1 なら、Blink の版の WebGL は ANGLE を OpenGL ES の裏で動かし、その下に Mesa(GL_LIB)を
# 結ぶ(設計書 14.6)。GL=1 でなければ WebGL 1 を CPU で描く
ifeq ($(GL),1)
BROWSER_GL     := GL_LIB=$(GL_LIB)
BROWSER_GL_DEP := $(GL_LIB)
endif

$(BROWSER_OUT)/browser.elf: $(CXXRT_LIB) $(PTH_LIB) $(UI_CXX_LIB) $(wildcard $(CHROMIUM)/application/browser/*.cc) \
		 $(CHROMIUM)/application/browser/br.h $(BROWSER_GL_DEP) browser-force
	@mkdir -p $(dir $@)
	@echo 'ENG $@'
	@TESSRONOS=$(abspath $(TOP)) LIBDIR=$(abspath $(CXXRT_OUT)) $(BROWSER_GL) $(BROWSER_ARCH_ENV) bash $(BROWSER_SH) $(BROWSER_STAGE) $(abspath $@)
