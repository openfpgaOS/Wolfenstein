# Wolfenstein / ECWolf — openfpgaOS custom core
#
# This Makefile builds a CUSTOM CORE: a standalone openFPGA core
# wrapping a single app. It owns its own dist/, build/, and ZIP.
#
# Targets:
#   make              Build the custom core
#   make debug        Build, push via UART, stream console
#   make copy         Copy this custom core to Pocket SD
#   make package      Package this custom core into a ZIP
#   make test         Test on desktop (SDL2)
#   make clean        Remove build artifacts
#

APP       = wolfenstein
TARGET   ?= pocket
MISTER_IP ?= mister.local
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(TARGET)/$(APP)
RUNTIME   = $(ROOT)/runtime
GAME_ASSETS = $(wildcard *.wl1 *.WL1 *.wl6 *.WL6 *.sod *.SOD *.sdm *.SDM *.sd2 *.SD2 *.sd3 *.SD3 *.n3d *.N3D *.pk3 *.PK3 *.ecwolf *.ECWOLF *.zip *.ZIP *.cue *.CUE *.bin *.BIN)

ECWOLF_SRC_DIR = port
WADSRC_STATIC_DIR = wadsrc/static
ZLIB_SRC_DIR   = zlib
LZMA_SRC_DIR   = lzma
BZIP2_SRC_DIR  = bzip2
BUILTIN_DIR    = $(OBJ_DIR)/builtin
BUILTIN_PK3    = $(BUILTIN_DIR)/ecwolf.pk3
BUILTIN_PK3_C  = $(BUILTIN_DIR)/ecwolf_builtin_data.c
BUILTIN_PK3_O  = $(BUILTIN_DIR)/ecwolf_builtin_data.o
WADSRC_FILES   = $(shell find $(WADSRC_STATIC_DIR) -type f | sort)
ECWOLF_INCLUDES = -I. -I$(ECWOLF_SRC_DIR) -I$(ECWOLF_SRC_DIR)/g_shared \
	-I$(ECWOLF_SRC_DIR)/g_wolf -I$(ECWOLF_SRC_DIR)/g_blake \
	-I$(ECWOLF_SRC_DIR)/r_2d -I$(ECWOLF_SRC_DIR)/r_data \
	-I$(ECWOLF_SRC_DIR)/resourcefiles -I$(ECWOLF_SRC_DIR)/sfmt \
	-I$(ECWOLF_SRC_DIR)/textures -I$(ECWOLF_SRC_DIR)/thingdef

SDL_TARGET_SRC = $(wildcard $(SDK_DIR)/of_sdl2.c)
OF_ECWOLF_SRC  = of_ecwolf_openfpga.c
OF_ECWOLF_CD_SRC = of_ecwolf_cd.cpp
OF_ECWOLF_CXXABI_SRC = of_ecwolf_cxxabi.cpp
OF_ECWOLF_GPU_SRC = of_ecwolf_gpu.cpp
ECWOLF_MIDI_SRC = $(SDK_DIR)/of_midi.c $(SDK_DIR)/of_smp_bank.c \
	$(SDK_DIR)/of_smp_voice.c $(SDK_DIR)/of_smp_tables.c
PC_EXTRA_CFLAGS += $(shell pkg-config --cflags SDL2_mixer 2>/dev/null)
PC_EXTRA_CFLAGS += $(shell pkg-config --cflags SDL2_net 2>/dev/null)
PC_EXTRA_LIBS += $(shell pkg-config --libs SDL2_mixer 2>/dev/null || echo -lSDL2_mixer)
PC_EXTRA_LIBS += $(shell pkg-config --libs SDL2_net 2>/dev/null)

CFLAGS   += -DOF_SDL2_EXTERNAL -DOF_ECWOLF_OPENFPGA \
	-DBZ_NO_STDIO -DUSE_GPL -DOF_ECWOLF_PERF_DISABLED \
	$(ECWOLF_INCLUDES) -I$(ZLIB_SRC_DIR) -I$(LZMA_SRC_DIR) -I$(BZIP2_SRC_DIR)
CFLAGS   += -Wno-old-style-definition -Wno-implicit-fallthrough \
	-Wno-unused-parameter -Wno-unused-function -Wno-unused-variable \
	-Wno-unused-but-set-variable
CXXFLAGS += -DOF_SDL2_EXTERNAL -DOF_ECWOLF_OPENFPGA \
	-DNO_GTK=1 -DUSE_GPL -DOF_ECWOLF_PERF_DISABLED
CXXFLAGS += -Dstricmp=strcasecmp -Dstrnicmp=strncasecmp
CXXFLAGS += -DBZ_NO_STDIO -Icxx_compat $(ECWOLF_INCLUDES) -I$(ZLIB_SRC_DIR) \
	-I$(LZMA_SRC_DIR) -I$(BZIP2_SRC_DIR) -std=gnu++11 -fexceptions
CXXFLAGS += -Wno-implicit-fallthrough -Wno-unused-parameter \
	-Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable \
	-Wno-missing-field-initializers -Wno-maybe-uninitialized \
	-Wno-class-memaccess -Wno-deprecated-copy
CFLAGS += -MMD -MP
CXXFLAGS += -MMD -MP

# Match Doom's tighter alignment.  The SDK default 64-byte alignment bloats
# ECWolf's much larger C++ renderer and wastes hot instruction-cache space.
CFLAGS += -falign-functions=16 -falign-loops=16
CXXFLAGS += -falign-functions=16 -falign-loops=16

# Preserve ECWolf's SDL-facing platform layer.  The openfpgaOS-specific
# implementation lives in the SDK SDL shim, not in the engine sources.
ECWOLF_C_SRCS = $(shell find $(ECWOLF_SRC_DIR) -type f -name '*.c' \
	! -path '*/android/*' ! -path '*/win32/*' ! -path '*/macosx/*' \
	! -path '*/dosbox/*' | sort)
ECWOLF_CXX_SRCS = $(shell find $(ECWOLF_SRC_DIR) -type f -name '*.cpp' \
	! -path '*/android/*' ! -path '*/win32/*' ! -path '*/macosx/*' \
	! -path '*/dosbox/*' ! -name 'wl_iwad_picker.cpp' \
	! -path '*/mame/fmopl.cpp' \
	! -path '*/resourcefiles/file_7z.cpp' \
	! -path '*/textures/jpegtexture.cpp' | sort)
ZLIB_SRCS = $(addprefix $(ZLIB_SRC_DIR)/, \
	adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c \
	inftrees.c trees.c uncompr.c zutil.c)
LZMA_SRCS = $(addprefix $(LZMA_SRC_DIR)/, CpuArch.c LzmaDec.c)
BZIP2_SRCS = $(addprefix $(BZIP2_SRC_DIR)/, \
	blocksort.c bzlib.c compress.c crctable.c decompress.c huffman.c randtable.c)

SRCS     = $(ZLIB_SRCS) $(LZMA_SRCS) $(BZIP2_SRCS) $(ECWOLF_C_SRCS) $(OF_ECWOLF_SRC) $(ECWOLF_MIDI_SRC) $(OF_EH_SRC)
# DBOPL (GPL) renders AdLib sound effects to PCM at load time; the dosbox/
# directory stays excluded from the generic scan, only the emulator builds.
ECWOLF_DBOPL_SRC = $(ECWOLF_SRC_DIR)/dosbox/dbopl.cpp
SRCS_CXX = $(ECWOLF_CXX_SRCS) $(ECWOLF_DBOPL_SRC) $(OF_ECWOLF_CD_SRC) $(OF_ECWOLF_CXXABI_SRC) $(OF_ECWOLF_GPU_SRC) $(OF_CXXABI_SRC)
PC_SRCS = $(filter-out $(ECWOLF_MIDI_SRC),$(SRCS))
PC_SRCS_CXX = $(filter-out $(OF_ECWOLF_CD_SRC) $(OF_ECWOLF_CXXABI_SRC) \
	$(OF_ECWOLF_GPU_SRC) $(OF_CXXABI_SRC),$(SRCS_CXX))
PC_CXXFLAGS = $(filter-out -Icxx_compat,$(CXXFLAGS))

# Declare default target before sdk.mk's rules
.DEFAULT_GOAL := all

# Containerized by default, like every other repo: the shared
# openfpgaos-firmware image bakes in xPack riscv-none-elf-gcc, whose rv32
# multilibs ship the real libsupc++ the EH machinery links (-lsupc++ in
# LIBS below — Ubuntu's gcc-riscv64-unknown-elf still has no bare-metal
# C++ runtime, so inside the container we pin CROSS to the xPack
# toolchain before sdk.mk's default can apply).
# USE_SDK_CONTAINER=0 remains the host-toolchain opt-out.
ifeq ($(OF_SDK_IN_CONTAINER),1)
CROSS := riscv-none-elf-
endif
USE_SDK_CONTAINER ?= 1

include $(SDK_DIR)/sdk.mk

-include $(APP_OBJS:.o=.d) $(OF_INIT_OBJ:.o=.d) $(OF_SDL2_OBJ:.o=.d)

STRIP ?= $(CROSS)strip

# ECWolf uses C++ exceptions for fatal/recoverable startup and resource errors.
# Keep the SDK's musl runtime, but pull in libsupc++ for the EH machinery.
LIBS = -Wl,--undefined=__register_frame_info \
	-Wl,--undefined=__deregister_frame_info \
	-Wl,--start-group -lc -lm -lsupc++ -lgcc -Wl,--end-group

APP_OBJS += $(BUILTIN_PK3_O)
$(OBJ_DIR)/app.elf: $(BUILTIN_PK3_O)

$(BUILTIN_PK3): $(WADSRC_FILES)
	@mkdir -p $(dir $@)
	@rm -f $@
	@cd $(WADSRC_STATIC_DIR) && find . -type f | sed 's#^\./##' | sort | zip -q -X $@ -@

$(BUILTIN_PK3_C): $(BUILTIN_PK3)
	@cd $(BUILTIN_DIR) && xxd -i ecwolf.pk3 | sed 's/^unsigned char/const unsigned char/' > $(notdir $@)

$(BUILTIN_PK3_O): $(BUILTIN_PK3_C)
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(CRT_DIR)/start.o: $(CRT_DIR)/start.S
	$(AS) $(ASFLAGS) -c -o $@ $<

# Build ELF, then assemble SD card image in build/<app>/
all: $(OBJ_DIR)/app.elf release
	@$(SIZE) $<

ifeq ($(TARGET),mister)
release: game-dist        # MiSTer: the per-game bundle IS the release artifact
else
release: $(OBJ_DIR)/app.elf
	@$(SDK_DIR)/platforms/$(TARGET)/image.sh "$(APP)" "$(OBJ_DIR)/app.elf" "$(ROOT)" "$(DIST)" $(GAME_ASSETS)
endif

COMMON = $(shell ls -d $(OUT)/Assets/*/common 2>/dev/null)

debug: all
	@$(ROOT)/scripts/debug.sh "$$(ls -d $(OUT)/Assets/*/common 2>/dev/null)/$(APP).elf"

copy: all
	@MISTER_IP="$(MISTER_IP)" $(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$(OBJ_DIR)/app.elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

test: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package test clean

# ── MiSTer per-game locked distribution ──────────────────────────────
# Mirrors src/doom/Makefile: read-only boot.vhd shell (the data files), a
# writable saves volume, one .mgl per instance and the loose F-loaded engine.
#
# Each data family ships as its OWN game bundle (own boot.vhd, wads/, saves,
# .mgl) instead of five instances sharing one image: the OS's file-name table
# holds 32 entries per boot, and one shared common/ with all five families'
# data sets was 47 files — the overflow was silent and the VSWAP.* family
# simply vanished, black-screening ECWolf before its first frame (2026-08-13).
# A family bundle is ~12 files, and users only download the families they own.
GAME_FAMILIES = Wolfenstein3D:wolf3d Spear:spear SpearM2:spear-m2 SpearM3:spear-m3 SuperNoah:noah
GAME_INIS   = $(DIST)/Assets/wolfenstein/common
GAME_STAGE  = $(ROOT)/build/mister/$(APP)
GAME_ELF    = wolfenstein.elf
GAME_CSV    = $(DIST)/mister/external_files.csv

game-dist: $(OBJ_DIR)/app.elf
	@rm -rf "$(GAME_STAGE)"
	@for fam in $(GAME_FAMILIES); do \
	  g=$${fam%%:*}; ini=$${fam##*:}; \
	  common="$(ROOT)/build/mister/.$(APP)-common-$$g"; \
	  inis="$(ROOT)/build/mister/.$(APP)-inis-$$g"; \
	  rm -rf "$$common" "$$inis"; \
	  mkdir -p "$(GAME_STAGE)/$$g/wads" "$$common" "$$inis"; \
	  cp "$(RUNTIME)/bank.ofsf" "$$common/bank.ofsf"; \
	  cp "$(GAME_INIS)/wolfmidi.zip" "$$common/" 2>/dev/null || true; \
	  cp "$(GAME_INIS)/sfxcache.ofx" "$$common/" 2>/dev/null || true; \
	  cp "$(GAME_INIS)/$$ini.ini" "$$inis/"; \
	  $(SDK_DIR)/platforms/mister/mkgame.sh --no-elf \
	      --saves-out "$(GAME_STAGE)/$$g/$$g.saves.vhd" \
	      "$$g" "$$inis" "$$common" "$(GAME_STAGE)/$$g/boot.vhd" || exit 1; \
	  $(SDK_DIR)/platforms/mister/mkmgl.sh "$$g" "$$inis" "$(GAME_STAGE)" || exit 1; \
	  cp "$(OBJ_DIR)/app.elf" "$(GAME_STAGE)/$$g/$(GAME_ELF)"; \
	  cp "$(GAME_INIS)/$$ini.ini" "$(GAME_STAGE)/$$g/"; \
	  sed "s|^GAME_DEFAULT=.*|GAME_DEFAULT=\"$$g\"|" \
	      "$(SDK_DIR)/platforms/mister/setup.sh" > "$(GAME_STAGE)/$$g/setup.sh"; \
	  chmod +x "$(GAME_STAGE)/$$g/setup.sh"; \
	  if [ -f "$(GAME_CSV)" ]; then cp "$(GAME_CSV)" "$(GAME_STAGE)/$$g/external_files.csv"; fi; \
	  rm -rf "$$common" "$$inis"; \
	done
	@echo "  game-dist staged -> $(GAME_STAGE) (families: $(GAME_FAMILIES))"
