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
TARGET    = pocket
SDK_DIR   = ../sdk
ROOT      = $(realpath $(CURDIR)/../..)
OBJ_DIR   = $(ROOT)/.obj/$(APP)
BUILD_DIR = $(OBJ_DIR)
DIST      = $(ROOT)/dist/$(APP)
OUT       = $(ROOT)/build/$(APP)
RUNTIME   = $(ROOT)/runtime

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

release: $(OBJ_DIR)/app.elf
	@rm -rf $(OUT)
	@mkdir -p $(dir $(OUT))
	@cp -r $(DIST) $(OUT)
	@cp $(RUNTIME)/bitstream.rbf_r $(RUNTIME)/loader.bin $$(ls -d $(OUT)/Cores/*/)/
	@mkdir -p $$(ls -d $(OUT)/Assets/*/)/common
	@cp $(RUNTIME)/os.bin $$(ls -d $(OUT)/Assets/*/)/common/
	@cp $(RUNTIME)/bank.ofsf $$(ls -d $(OUT)/Assets/*/)/common/ 2>/dev/null || true
	@common=$$(ls -d $(OUT)/Assets/*/)/common; \
		$(STRIP) -o $$common/$(APP).elf $<
	@for f in *.wl1 *.WL1 *.wl6 *.WL6 *.sod *.SOD *.sdm *.SDM *.sd2 *.SD2 *.sd3 *.SD3 *.n3d *.N3D *.pk3 *.PK3 *.ecwolf *.ECWOLF *.zip *.ZIP *.cue *.CUE *.bin *.BIN; do [ -f "$$f" ] && cp "$$f" $$(ls -d $(OUT)/Assets/*/)/common/; done 2>/dev/null; true
	@echo "Ready: build/$(APP)/"

COMMON = $(shell ls -d $(OUT)/Assets/*/common 2>/dev/null)

debug: all
	@$(ROOT)/scripts/debug.sh $$(ls -d $(OUT)/Assets/*/common)/$(APP).elf

copy: all
	@$(SDK_DIR)/platforms/$(TARGET)/copy.sh "$(APP)" "$$(ls -d $(OUT)/Assets/*/common)/$(APP).elf"

package: all
	@$(ROOT)/scripts/package.sh $(APP)

test: app_pc

clean: sdk-clean
	rm -rf $(OUT)

.PHONY: all release debug copy package test clean
