# halo-ps2 engine — EE build (ps2sdk conventions)
PS2DEV ?= $(HOME)/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
GSKIT  ?= $(PS2DEV)/gsKit
export PS2DEV PS2SDK GSKIT
export PATH := $(PS2DEV)/bin:$(PS2DEV)/ee/bin:$(PS2DEV)/iop/bin:$(PS2DEV)/dvp/bin:$(PS2SDK)/bin:$(PATH)

EE_BIN  = halo-ps2.elf
EE_OBJS = src/main.o \
          src/core/config.o \
          src/core/mem.o \
          src/platform/fbdump.o \
          src/platform/pad.o \
          src/platform/profiler.o \
          src/platform/video.o \
          src/render/bsp_render.o \
          src/render/camera.o \
          src/render/chain.o \
          src/render/clip.o \
          src/render/flycam.o \
          src/render/selftest.o \
          src/render/text.o \
          src/render/texcache.o \
          src/render/upload.o \
          src/render/vis.o \
          src/stream/file.o \
          src/stream/hp2.o \
          src/vu1/bsp.o \
          src/world/bsp.o \
          src/world/tex.o \
          src/world/world.o
EE_LIBS = -ldraw -lgraph -lpacket2 -ldma -ldebug -lpad -lm
EE_INCS = -Isrc
EE_CFLAGS = -std=gnu11 -Wextra -Wno-unused-parameter -MMD -MP
EE_DVP  = dvp-as

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)
	@$(EE_TOOL_PREFIX)size $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

clean:
	rm -f $(EE_BIN) $(EE_OBJS) $(EE_OBJS:.o=.d)

run: $(EE_BIN)
	../scripts/run-pcsx2.sh $(EE_BIN)

hw: $(EE_BIN)
	ps2client -h $(PS2_IP) execee host:$(EE_BIN)

.PHONY: all clean run hw

-include $(EE_OBJS:.o=.d)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
