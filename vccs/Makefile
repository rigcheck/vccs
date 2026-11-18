# VCCS - OpenOrbis build (Linux/macOS)
#
# Steps mirror the toolchain's samples/hello_world Makefile:
#   clang -> ld.lld -> create-fself --paid -> PkgTool.Core sfo_* -> create-gp4 -> PkgTool.Core pkg_build
#
#   export OO_PS4_TOOLCHAIN=/path/to/OpenOrbis/PS4Toolchain
#   make                 -> pkg/IV0000-VCCS00001_00-VCCS000000000000.pkg
#   make plugin          -> build/vccs_plugin.prx (background plugin)
#   make host-test       -> logic tests on your PC (no SDK needed)
#
# If your toolchain keeps right.sprx somewhere else, run:
#   make RIGHT_SPRX=/path/to/right.sprx

TITLE      := VCCS
TITLE_ID   := VCCS00001
CONTENT_ID := IV0000-$(TITLE_ID)_00-VCCS000000000000
VERSION    := 01.00

TOOLCHAIN  := $(OO_PS4_TOOLCHAIN)
UNAME      := $(shell uname -s)
ifeq ($(UNAME),Linux)
HOSTDIR := linux
else
HOSTDIR := macos
endif
BIN := $(TOOLCHAIN)/bin/$(HOSTDIR)

CC := clang
LD := ld.lld
ifeq ($(UNAME),Darwin)
CC := /usr/local/opt/llvm/bin/clang
LD := /usr/local/opt/llvm/bin/ld.lld
endif

RIGHT_SPRX ?= $(TOOLCHAIN)/samples/hello_world/sce_sys/about/right.sprx

CFLAGS  := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -O2 -Wall -c \
           -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include
LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x --eh-frame-hdr -L$(TOOLCHAIN)/lib
LIBS    := -lc -lkernel -lSceVideoOut -lScePad -lSceUserService -lSceSystemService

OUT    := build
STAGE  := $(OUT)/pkgroot
PKG    := pkg/$(CONTENT_ID).pkg

all: $(PKG)

$(OUT)/main.o: src/main.c
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -o $@ $<

$(OUT)/lightbar.elf: $(OUT)/main.o
	$(LD) $(LDFLAGS) $(TOOLCHAIN)/lib/crt1.o $< $(LIBS) -o $@

$(OUT)/eboot.bin: $(OUT)/lightbar.elf
	$(BIN)/create-fself -in=$< -out=$(OUT)/lightbar.oelf --eboot=$@ --paid 0x3800000000000011

$(STAGE)/sce_sys/param.sfo:
	@mkdir -p $(STAGE)/sce_sys
	$(BIN)/PkgTool.Core sfo_new $@
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_TYPE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ APP_VER --type Utf8 --maxsize 8 --value '$(VERSION)'
	$(BIN)/PkgTool.Core sfo_setentry $@ ATTRIBUTE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ CATEGORY --type Utf8 --maxsize 4 --value 'gd'
	$(BIN)/PkgTool.Core sfo_setentry $@ CONTENT_ID --type Utf8 --maxsize 48 --value '$(CONTENT_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ DOWNLOAD_DATA_SIZE --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ SYSTEM_VER --type Integer --maxsize 4 --value 0
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE --type Utf8 --maxsize 128 --value '$(TITLE)'
	$(BIN)/PkgTool.Core sfo_setentry $@ TITLE_ID --type Utf8 --maxsize 12 --value '$(TITLE_ID)'
	$(BIN)/PkgTool.Core sfo_setentry $@ VERSION --type Utf8 --maxsize 8 --value '$(VERSION)'

$(PKG): $(OUT)/eboot.bin $(STAGE)/sce_sys/param.sfo sce_sys/icon0.png
	@test -f "$(RIGHT_SPRX)" || { echo "right.sprx not found at $(RIGHT_SPRX) - pass RIGHT_SPRX=..."; exit 1; }
	@mkdir -p $(STAGE)/sce_sys/about pkg
	cp $(OUT)/eboot.bin $(STAGE)/eboot.bin
	cp sce_sys/icon0.png $(STAGE)/sce_sys/icon0.png
	cp "$(RIGHT_SPRX)" $(STAGE)/sce_sys/about/right.sprx
	cd $(STAGE) && $(BIN)/create-gp4 -out ../pkg.gp4 --content-id=$(CONTENT_ID) \
	    --files "eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png"
	cd $(OUT) && $(BIN)/PkgTool.Core pkg_build pkg.gp4 ../pkg

# ---- background plugin: mirrors the build rules of the GoldHEN Plugin SDK's own Makefile ----
SDK_DIR     := third_party/GoldHEN_Plugins_SDK
SDK_SRC     := $(wildcard $(SDK_DIR)/source/*.c)
SDK_OBJ     := $(patsubst $(SDK_DIR)/source/%.c,$(OUT)/sdk/%.o,$(SDK_SRC))
PLUGIN_LIBS := -lSceLibcInternal -lkernel -lSceSysmodule -lScePad -lSceUserService
PLUGIN_CFLAGS := --target=x86_64-pc-freebsd12-elf -fPIC -funwind-tables -c -D__USE_KLOG__ -DDEBUG=0 \
           -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -I$(SDK_DIR)/include
PLUGIN_LDFLAGS := -m elf_x86_64 -pie --script $(TOOLCHAIN)/link.x -e _init --eh-frame-hdr -L$(TOOLCHAIN)/lib

$(OUT)/sdk/%.o: $(SDK_DIR)/source/%.c
	@mkdir -p $(OUT)/sdk
	$(CC) $(PLUGIN_CFLAGS) -o $@ $<

$(OUT)/crtprx.o: $(SDK_DIR)/crt/crtprx.c
	@mkdir -p $(OUT)
	$(CC) -target x86_64-pc-linux-gnu -ffreestanding -nostdlib -fno-builtin -fPIC -isysroot $(TOOLCHAIN) -isystem $(TOOLCHAIN)/include -c $< -o $@

$(OUT)/vccs_plugin.o: plugin/vccs_plugin.c
	@mkdir -p $(OUT)
	$(CC) $(PLUGIN_CFLAGS) -o $@ $<

$(OUT)/vccs_plugin.elf: $(OUT)/vccs_plugin.o $(SDK_OBJ) $(OUT)/crtprx.o
	$(LD) $(SDK_OBJ) $(OUT)/vccs_plugin.o $(OUT)/crtprx.o -o $@ $(PLUGIN_LDFLAGS) $(PLUGIN_LIBS)

$(OUT)/vccs_plugin.prx: $(OUT)/vccs_plugin.elf
	$(BIN)/create-fself -in=$< -out=$(OUT)/vccs_plugin.oelf --lib=$@ --paid 0x3800000000000011

# background plugin (keeps the light bar after leaving the app); copy to /data/GoldHEN/plugins/
plugin: $(OUT)/vccs_plugin.prx

host-test:
	@mkdir -p $(OUT)/cfgtest
	gcc -Wall -Wextra -O1 -DHOST_TEST -DCFG_DIR='"$(OUT)/cfgtest"' -Isrc -Itests tests/host_test.c -o $(OUT)/host_test
	$(OUT)/host_test
	gcc -Wall -Wextra -O1 -DHOST_TEST -DCFG_DIR='"$(OUT)/cfgtest"' -Itests -I$(SDK_DIR)/include tests/plugin_test.c $(SDK_DIR)/source/HDE64.c -o $(OUT)/plugin_test
	$(OUT)/plugin_test

clean:
	rm -rf $(OUT) pkg

.PHONY: all plugin host-test clean
