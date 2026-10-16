# halo-ps2 engine — EE build (ps2sdk conventions)
PS2DEV ?= $(HOME)/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
GSKIT  ?= $(PS2DEV)/gsKit
export PS2DEV PS2SDK GSKIT
export PATH := $(PS2DEV)/bin:$(PS2DEV)/ee/bin:$(PS2DEV)/iop/bin:$(PS2DEV)/dvp/bin:$(PS2SDK)/bin:$(PATH)

EE_BIN  = halo-ps2.elf
EE_OBJS = src/main.o \
          src/platform/video.o \
          src/render/vu1_tri.o \
          src/stream/hp2.o \
          src/world/bsp.o \
          src/vu1/tri.o
EE_LIBS = -ldraw -lgraph -lmath3d -lpacket2 -ldma
EE_INCS = -Isrc
EE_CFLAGS = -std=gnu11 -Wextra -Wno-unused-parameter
EE_DVP  = dvp-as

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)
	@$(EE_TOOL_PREFIX)size $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

run: $(EE_BIN)
	../scripts/run-pcsx2.sh $(EE_BIN)

hw: $(EE_BIN)
	ps2client -h $(PS2_IP) execee host:$(EE_BIN)

.PHONY: all clean run hw

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
