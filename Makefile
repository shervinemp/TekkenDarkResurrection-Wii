#---------------------------------------------------------------------------------
# Tekken: Dark Resurrection Wii - devkitPPC build
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITPPC)),)
$(error "Please set DEVKITPPC (devkitPro/devkitPPC) in your environment")
endif

include $(DEVKITPPC)/wii_rules

# Historical output name; the app is Tekken: Dark Resurrection.
TARGET      := tekken6wii
BUILD       := build
OBJ         := $(BUILD)/obj
PYTHON      ?= python

# If build/gen exists (tools/prepare_game.py produced it), use it.  Otherwise
# build a launcher-only DOL with an empty function table; it boots, explains
# what is missing, and never pretends the game can run.
GEN_DIR     := $(if $(wildcard gen/func_table.c),gen,stub)

CFLAGS      := -g -O2 -Wall -ffunction-sections -fdata-sections \
               -fno-strict-aliasing -fwrapv -Wno-unused-variable \
               -fno-asynchronous-unwind-tables -fno-unwind-tables \
               -Wno-unused-label -Wno-unused-function \
               -Wno-tautological-compare -Wno-format -Wno-implicit-fallthrough \
               -D__wii__ $(MACHDEP) -I$(LIBOGC_INC) \
               -Iplatform/wii -Iruntime -I$(GEN_DIR) \
               -Ithird_party/at3_standalone -Ithird_party/at3_standalone/shim \
               -Ithird_party/stb -Ivendor/libntfs/include -Ivendor/libntfs/source
ifdef WII_OSREPORT
CFLAGS += -DWII_OSREPORT
endif

CXXFLAGS    := $(CFLAGS) -std=gnu++17 -fno-exceptions -fno-rtti -Wno-parentheses-equality

# Optional extra flags, for the performance experiments.  Empty by default.
CFLAGS      += $(EXTRA_CFLAGS)

# Debug-only instrumentation (HLE trace ring, host stack watermark).  The
# production target never defines it, so the shipped DOL stays free of probes.
ifneq (,$(findstring debug,$(TARGET)))
CFLAGS      += -DHLE_TRACE
endif

LDFLAGS     := -g $(MACHDEP) -Wl,-Map,$(TARGET).map -Wl,--gc-sections -Wl,--allow-multiple-definition
LIBS        := -lfat -laesnd -lwiiuse -lbte -logc -lm

C_SOURCES   := $(wildcard platform/wii/*.c) \
               $(wildcard runtime/wii/*.c) \
               $(wildcard runtime/hle/*.c) \
               $(wildcard runtime/gpu/*.c) \
               runtime/vfpu.c \
               $(wildcard vendor/libntfs/source/*.c)
C_SOURCES   := $(filter-out runtime/gpu/glr.c,$(C_SOURCES))
# The intro movie: H.264 through FFmpeg's decoder when tools/build_ffmpeg.sh has built it for the Wii (third_party/ffmpeg-ppc,
# ignored by git); otherwise the stub that draws black.  `make EXP_NOFFMPEG=1` forces the stub.
FFMPEG_DIR  := third_party/ffmpeg-ppc
ifndef EXP_NOFFMPEG
ifneq ($(wildcard $(FFMPEG_DIR)/lib/libavcodec.a),)
USE_FFMPEG  := 1
endif
endif
ifdef USE_FFMPEG
C_SOURCES   := $(filter-out runtime/hle/avc_stub.c,$(C_SOURCES))
CFLAGS      += -DTKDR_FFMPEG -I$(FFMPEG_DIR)/include
LIBS        := -L$(FFMPEG_DIR)/lib -lavcodec -lavutil $(LIBS)
else
C_SOURCES   := $(filter-out runtime/hle/avc.c runtime/hle/avc_mem.c,$(C_SOURCES))
endif
CPP_SOURCES := $(wildcard runtime/hle/*.cpp) $(wildcard third_party/at3_standalone/*.cpp)
GEN_SOURCES := $(wildcard $(GEN_DIR)/*.c)

OBJECTS     := $(patsubst %.c,$(OBJ)/%.o,$(C_SOURCES)) \
               $(patsubst %.cpp,$(OBJ)/%.o,$(CPP_SOURCES)) \
               $(patsubst %.c,$(OBJ)/%.o,$(GEN_SOURCES))

.PHONY: all clean run dist-tkdr
all: $(TARGET).dol

# libogc keeps r2 at the small-data base for every thread, so __thread data is
# not per-thread: every thread-local variable in the program ends up in one
# shared word.  Refuse to link anything that has some.
$(TARGET).elf: $(OBJECTS)
	$(SILENTMSG) linking ... $(notdir $@)
	$(SILENTCMD)$(CXX) $^ $(LDFLAGS) -L$(LIBOGC_LIB) $(LIBS) -o $@
	@if $(DEVKITPPC)/bin/powerpc-eabi-readelf -SW $@ | grep -q -E '\.t(bss|data)'; then \
		echo "error: $@ has thread-local data, which libogc does not support" >&2; \
		rm -f $@; exit 1; fi

$(TARGET).dol: $(TARGET).elf
	$(SILENTMSG) output ... $(notdir $@)
	$(SILENTCMD)elf2dol $< $@.raw
	$(SILENTCMD)$(PYTHON) tools/pad_dol.py $@.raw $@ --data
	$(SILENTCMD)rm -f $@.raw

$(OBJ)/%.o: %.c
	@mkdir -p $(dir $@)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)$(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS) -c $< -o $@

# GCC 15.2 dies in try_forward_edges on raster.c now and then, and the same
# source compiles on the next try: so it is tried up to three times.
$(OBJ)/runtime/gpu/raster.o: runtime/gpu/raster.c
	@mkdir -p $(dir $@)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)for n in 1 2 3; do $(CC) -MMD -MP -MF $(@:.o=.d) $(CFLAGS) -c $< -o $@ && exit 0; done; exit 1

$(OBJ)/%.o: %.cpp
	@mkdir -p $(dir $@)
	$(SILENTMSG) $(notdir $<)
	$(SILENTCMD)$(CXX) -MMD -MP -MF $(@:.o=.d) $(CXXFLAGS) -c $< -o $@

# libntfs is a pre-C11 portable port; GCC 15 needs the old semantics.
$(OBJ)/vendor/libntfs/source/%.o: CFLAGS += -DHAVE_CONFIG_H -DNTFS_ENABLE_LOG \
                                        -fcommon -std=gnu99
# GCC 15.2 ICEs in cprop/try_forward_edges (cfgcleanup.cc) on this unit at
# -O2.  The failure moves from function to function, so the whole unit is
# built at -O1, as draw_line and texreplace.c already are.
$(OBJ)/runtime/gpu/raster.o: CFLAGS += -O1 -fno-move-loop-invariants -fno-move-loop-stores -fno-thread-jumps -fno-crossjumping -fno-guess-branch-probability
# The save/utility dialog code triggers the same compiler failure and is not
# on the frame path.
$(OBJ)/runtime/hle/utility.o: CFLAGS += -O1
# Generated game code is huge.  Keep the size-optimised default for the
# shipped DOL, but allow a speed build to be measured with GEN_OPT=-O2.
GEN_OPT ?= -Os
$(OBJ)/gen/%.o: CFLAGS += $(GEN_OPT) -Wno-unused-but-set-variable
# the functions the guest profile found hot (profiles/tkdr/config/hot_functions.txt, when present) are built for speed
HOT_OPT ?= -O2
$(OBJ)/gen/funcs_hot.o: CFLAGS += $(HOT_OPT)
$(OBJ)/stub/%.o: CFLAGS += -Os

run: $(TARGET).dol
	wiiload $(TARGET).dol

clean:
	rm -rf $(BUILD) $(TARGET).elf $(TARGET).dol $(TARGET).dol.raw $(TARGET).map

-include $(OBJECTS:.o=.d)

dist-tkdr: $(TARGET).dol
	rm -rf dist-tkdr
	mkdir -p dist-tkdr/apps/tkdr
	cp $(TARGET).dol dist-tkdr/apps/tkdr/boot.dol
	cp wii/apps/tkdr/meta.xml wii/apps/tkdr/README.txt dist-tkdr/apps/tkdr/
	if [ -f gen/image.bin ]; then cp gen/image.bin dist-tkdr/apps/tkdr/; fi