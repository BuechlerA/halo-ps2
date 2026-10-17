# Console build of the streamed-audio test ELF (docs/29-strm-section.md, "EE verification"): a tiny program that opens a .strm.hp2, plays a music loop,
# a dialogue line and a looping ambience through audsrv and prints counters. Not part of the engine build and not run by CI:
#   make -C engine/tests/ee_strm && scripts/run-headless.sh -t 60 -c engine/tests/ee_strm/strm.cfg engine/tests/ee_strm/strmtest.elf
# (run-headless greps only the engine's own tags: add -l emu.log and grep '\[stream\]' there). Needs assets/hp2/<map>.strm.hp2 (streamconv.py).
PS2DEV ?= $(HOME)/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
export PS2DEV PS2SDK
export PATH := $(PS2DEV)/bin:$(PS2DEV)/ee/bin:$(PS2DEV)/iop/bin:$(PS2DEV)/dvp/bin:$(PS2SDK)/bin:$(PATH)
ENG = ../../src
EE_BIN = strmtest.elf
EE_OBJS = main.o audio.o audsrv_irx.o stream.o strmdef.o file.o hp2.o mem.o config.o
EE_LIBS = -ldebug -laudsrv -lpatches -lm
EE_INCS = -I$(ENG)
EE_CFLAGS = -std=gnu11 -Wextra -Wno-unused-parameter -MMD -MP

all: $(EE_BIN)

# objects are built here, from the engine's sources (the engine's own objects are not touched)
%.o: $(ENG)/audio/%.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@
%.o: $(ENG)/stream/%.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@
%.o: $(ENG)/core/%.c
	$(EE_CC) $(EE_CFLAGS) $(EE_INCS) -c $< -o $@

# the IOP audio server comes from the SDK (as in engine/Makefile)
audsrv_irx.c:
	bin2c $(PS2SDK)/iop/irx/audsrv.irx $@ audsrv_irx

clean:
	rm -f audsrv_irx.c $(EE_BIN) $(EE_OBJS) $(EE_OBJS:.o=.d)

.PHONY: all clean
-include $(EE_OBJS:.o=.d)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
