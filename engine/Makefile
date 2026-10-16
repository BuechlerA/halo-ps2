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

# Host-compiled unit tests for the platform-independent parts (no ps2sdk needed).
test:
	HP2_FIXTURE=$(HOST_TMP)/halo-test.hp2 python3 -I ../tools/tests/test_hp2.py
	cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Isrc \
		tests/test_hp2.c src/stream/hp2.c -o $(HOST_TMP)/test_hp2
	$(HOST_TMP)/test_hp2 $(HOST_TMP)/halo-test.hp2

hw: $(EE_BIN)
	ps2client -h $(PS2_IP) execee host:$(EE_BIN)

HOST_TMP ?= $(or $(TMPDIR),/tmp)
.PHONY: all clean run hw test

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
