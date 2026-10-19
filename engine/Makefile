# halo-ps2 engine — EE build (ps2sdk conventions)
PS2DEV ?= $(HOME)/ps2dev
PS2SDK ?= $(PS2DEV)/ps2sdk
GSKIT  ?= $(PS2DEV)/gsKit
export PS2DEV PS2SDK GSKIT
export PATH := $(PS2DEV)/bin:$(PS2DEV)/ee/bin:$(PS2DEV)/iop/bin:$(PS2DEV)/dvp/bin:$(PS2SDK)/bin:$(PATH)

EE_BIN  = halo-ps2.elf
EE_OBJS = src/main.o \
          src/audio/strmdef.o \
          src/audio/stream.o \
          src/mission_app.o \
          src/script/hs_host_ai.o \
          src/script/hs_host_misc.o \
          src/script/hs_host_obj.o \
          src/ai/ai.o \
          src/ai/aidef.o \
          src/ai/path.o \
          src/audio/audio.o \
          src/audio/gameaudio.o \
          src/audio/snddef.o \
          src/audio/audsrv_irx.o \
          src/core/config.o \
          src/core/mat.o \
          src/core/mem.o \
          src/platform/fbdump.o \
          src/platform/memcard.o \
          src/platform/pad.o \
          src/platform/profiler.o \
          src/platform/video.o \
          src/script/hs.o \
          src/script/hs_core.o \
          src/sim/cine.o \
          src/sim/devices.o \
          src/sim/fpview.o \
          src/sim/fx.o \
          src/sim/bot.o \
          src/sim/game.o \
          src/sim/mission.o \
          src/sim/msave.o \
          src/sim/input.o \
          src/sim/phys.o \
          src/sim/player.o \
          src/sim/playerdef.o \
          src/sim/save.o \
          src/sim/unitdef.o \
          src/ai/units.o \
          src/sim/vitals.o \
          src/sim/weapdef.o \
          src/sim/objects.o \
          src/sim/skeleton.o \
          src/render/bsp_render.o \
          src/render/camera.o \
          src/render/chain.o \
          src/render/clip.o \
          src/render/flycam.o \
          src/render/fx_render.o \
          src/render/hud_render.o \
          src/render/lodctl.o \
          src/render/obj_render.o \
          src/render/selftest.o \
          src/render/sprite.o \
          src/render/text.o \
          src/render/texcache.o \
          src/render/upload.o \
          src/render/vis.o \
          src/stream/file.o \
          src/stream/hp2.o \
          src/vu1/world.o \
          src/ui/hud.o \
          src/ui/huddef.o \
          src/world/anim.o \
          src/world/ansm.o \
          src/world/bsp.o \
          src/world/cinedef.o \
          src/world/cchg.o \
          src/world/cmod.o \
          src/world/devdef.o \
          src/world/coll.o \
          src/world/fxdef.o \
          src/world/gamedata.o \
          src/world/model.o src/world/mouth.o \
          src/world/scen.o \
          src/world/tex.o \
          src/world/world.o
EE_LIBS = -ldraw -lgraph -lpacket2 -ldma -ldebug -lpad -lmc -laudsrv -lpatches -lm
EE_INCS = -Isrc
EE_CFLAGS = -std=gnu11 -Wextra -Wno-unused-parameter -MMD -MP
EE_DVP  = dvp-as

all: $(EE_BIN)
	$(EE_STRIP) --strip-all $(EE_BIN)
	@$(EE_TOOL_PREFIX)size $(EE_BIN)

%.o: %.vsm
	$(EE_DVP) $< -o $@

src/vu1/world.o: src/vu1/bsp.vsm src/vu1/model.vsm

# ps2sdk's stock IOP audio server is embedded in the ELF (loaded with SifExecModuleBuffer): ADR-030
src/audio/audsrv_irx.c:
	bin2c $(PS2SDK)/iop/irx/audsrv.irx $@ audsrv_irx

clean:
	rm -f src/audio/audsrv_irx.c $(EE_BIN) $(EE_OBJS) $(EE_OBJS:.o=.d)

run: $(EE_BIN)
	../scripts/run-pcsx2.sh $(EE_BIN)

hw: $(EE_BIN)
	ps2client -h $(PS2_IP) execee host:$(EE_BIN)

.PHONY: all clean run hw

-include $(EE_OBJS:.o=.d)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
