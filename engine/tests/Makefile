# Host-compiled unit tests for the platform-independent engine code. Needs only python3 and a
# C compiler (no ps2dev), so CI runs it on a stock runner:  make -C engine/tests
HOST_TMP ?= $(or $(TMPDIR),/tmp)
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I../src
PYTESTS  = ../../tools/tests
STRM_REAL = $(wildcard ../../assets/hp2/*.strm.hp2)
STRM_ARGS = $(foreach p,$(STRM_REAL),$(p)=$(HOST_TMP)/$(notdir $(p)).crc)

test: $(HOST_TMP)/halo-test.hp2 $(HOST_TMP)/halo-bsp.hp2
	cc $(CFLAGS) test_hp2.c ../src/stream/hp2.c -o $(HOST_TMP)/test_hp2
	$(HOST_TMP)/test_hp2 $(HOST_TMP)/halo-test.hp2
	cc $(CFLAGS) test_bsp.c ../src/world/bsp.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_bsp
	$(HOST_TMP)/test_bsp $(HOST_TMP)/halo-bsp.hp2
	TEX_FIXTURE=$(HOST_TMP)/halo-tex.hp2 python3 -I $(PYTESTS)/test_texconv.py && cc $(CFLAGS) test_tex.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_tex && $(HOST_TMP)/test_tex $(HOST_TMP)/halo-tex.hp2
	cc $(CFLAGS) test_render.c ../src/core/mem.c ../src/render/camera.c -lm -o $(HOST_TMP)/test_render
	$(HOST_TMP)/test_render
	cc $(CFLAGS) test_config.c ../src/core/config.c -lm -o $(HOST_TMP)/test_config
	$(HOST_TMP)/test_config
	cc $(CFLAGS) test_texcache.c ../src/render/texcache.c -o $(HOST_TMP)/test_texcache
	$(HOST_TMP)/test_texcache
	cc $(CFLAGS) test_cchg.c ../src/world/cchg.c -o $(HOST_TMP)/test_cchg
	$(HOST_TMP)/test_cchg
	cc $(CFLAGS) test_euler.c ../src/core/mat.c ../src/sim/cine.c ../src/world/cinedef.c ../src/stream/hp2.c ../src/render/camera.c -lm -o $(HOST_TMP)/test_euler
	$(HOST_TMP)/test_euler
	cc $(CFLAGS) test_chain.c ../src/render/chain.c ../src/render/upload.c -o $(HOST_TMP)/test_chain
	$(HOST_TMP)/test_chain
	cc $(CFLAGS) test_clip.c ../src/render/clip.c ../src/render/camera.c -lm -o $(HOST_TMP)/test_clip
	$(HOST_TMP)/test_clip
	cc $(CFLAGS) test_text.c ../src/render/text.c ../src/render/chain.c -o $(HOST_TMP)/test_text
	$(HOST_TMP)/test_text
	cc $(CFLAGS) test_flycam.c ../src/render/flycam.c ../src/render/camera.c ../src/core/config.c -lm -o $(HOST_TMP)/test_flycam
	$(HOST_TMP)/test_flycam
	cc $(CFLAGS) test_skeleton.c ../src/core/mat.c ../src/sim/skeleton.c ../src/render/camera.c -lm -o $(HOST_TMP)/test_skeleton
	$(HOST_TMP)/test_skeleton
	cc $(CFLAGS) test_lodctl.c ../src/render/lodctl.c -o $(HOST_TMP)/test_lodctl
	$(HOST_TMP)/test_lodctl
	SCEN_FIXTURE=$(HOST_TMP)/halo-scen.hp2 python3 -I $(PYTESTS)/test_scenconv.py && cc $(CFLAGS) test_scen.c ../src/world/scen.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_scen && $(HOST_TMP)/test_scen $(HOST_TMP)/halo-scen.hp2
	MODEL_FIXTURE=$(HOST_TMP)/halo-model.hp2 python3 -I $(PYTESTS)/test_modelconv.py && cc $(CFLAGS) test_model.c ../src/world/model.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_model && $(HOST_TMP)/test_model $(HOST_TMP)/halo-model.hp2
	ANIM_FIXTURE=$(HOST_TMP)/halo-anim.hp2 python3 -I $(PYTESTS)/test_animconv.py && cc $(CFLAGS) test_anim.c ../src/world/anim.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_anim && $(HOST_TMP)/test_anim $(HOST_TMP)/halo-anim.hp2
	MOUT_FIXTURE=$(HOST_TMP)/halo-mout.bin python3 -I $(PYTESTS)/test_mouthconv.py && cc $(CFLAGS) test_mouth.c ../src/world/mouth.c -o $(HOST_TMP)/test_mouth && $(HOST_TMP)/test_mouth $(HOST_TMP)/halo-mout.bin
	HIRES_FIXTURE=$(HOST_TMP)/halo-hires.hp2 python3 -I $(PYTESTS)/test_objtex_hero.py && cc $(CFLAGS) test_hires.c ../src/world/hires.c ../src/world/tex.c ../src/stream/hp2.c ../src/core/mem.c -lm -o $(HOST_TMP)/test_hires && $(HOST_TMP)/test_hires $(HOST_TMP)/halo-hires.hp2
	FXDEF_FIXTURE=$(HOST_TMP)/halo-fxdef.hp2 python3 -I $(PYTESTS)/test_fxconv.py && cc $(CFLAGS) test_fxdef.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_fxdef && $(HOST_TMP)/test_fxdef $(HOST_TMP)/halo-fxdef.hp2
	cc $(CFLAGS) test_fxat.c ../src/sim/fx.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_fxat && $(HOST_TMP)/test_fxat
	ANSM_FIXTURE=$(HOST_TMP)/halo-ansm.hp2 python3 -I $(PYTESTS)/test_ansmconv.py && cc $(CFLAGS) test_ansm.c ../src/world/ansm.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_ansm && $(HOST_TMP)/test_ansm $(HOST_TMP)/halo-ansm.hp2
	UNIT_FIXTURE=$(HOST_TMP)/halo-unit.hp2 python3 -I $(PYTESTS)/test_unitconv.py && cc $(CFLAGS) test_unitdef.c ../src/sim/unitdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_unitdef && $(HOST_TMP)/test_unitdef $(HOST_TMP)/halo-unit.hp2 $(wildcard ../../assets/hp2/a10.unit.hp2)
	CMOD_FIXTURE=$(HOST_TMP)/halo-cmod.hp2 python3 -I $(PYTESTS)/test_cmodconv.py && cc $(CFLAGS) test_cmod.c ../src/world/cmod.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_cmod && $(HOST_TMP)/test_cmod $(HOST_TMP)/halo-cmod.hp2
	SND_FIXTURE=$(HOST_TMP)/halo-snd.hp2 python3 -I $(PYTESTS)/test_soundconv.py && cc $(CFLAGS) test_snddef.c ../src/audio/snddef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_snddef && $(HOST_TMP)/test_snddef $(HOST_TMP)/halo-snd.hp2 $(wildcard ../../assets/hp2/*.snd.hp2)
	STRM_FIXTURE=$(HOST_TMP)/halo-strm.hp2 python3 -I $(PYTESTS)/test_streamconv.py && cc $(CFLAGS) test_strmdef.c ../src/audio/strmdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_strmdef && for p in $(STRM_REAL); do python3 -I ../../tools/halo2ps2/streamconv.py --verify $$p --samples 60 --crc-out $(HOST_TMP)/$$(basename $$p).crc > /dev/null || exit 1; done && $(HOST_TMP)/test_strmdef $(HOST_TMP)/halo-strm.hp2 $(STRM_ARGS)
	cc $(CFLAGS) test_stream.c ../src/audio/stream.c ../src/audio/strmdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_stream && $(HOST_TMP)/test_stream $(HOST_TMP)/halo-strm.hp2 $(STRM_REAL)
	WEAP_FIXTURE=$(HOST_TMP)/halo-weap.hp2 python3 -I $(PYTESTS)/test_weapconv.py && cc $(CFLAGS) test_weapdef.c ../src/sim/weapdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_weapdef && $(HOST_TMP)/test_weapdef $(HOST_TMP)/halo-weap.hp2 $(wildcard ../../assets/hp2/*.weap.hp2)
	HUD_FIXTURE=$(HOST_TMP)/halo-hud.hp2 python3 -I $(PYTESTS)/test_hudconv.py && cc $(CFLAGS) test_huddef.c ../src/ui/huddef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_huddef && $(HOST_TMP)/test_huddef $(HOST_TMP)/halo-hud.hp2
	AI_FIXTURE=$(HOST_TMP)/halo-ai.hp2 python3 -I $(PYTESTS)/test_aiconv.py && cc $(CFLAGS) test_aidef.c ../src/ai/aidef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_aidef && $(HOST_TMP)/test_aidef $(HOST_TMP)/halo-ai.hp2 $(wildcard ../../assets/hp2/*.ai.hp2)
	DEVS_FIXTURE=$(HOST_TMP)/halo-devs.hp2 python3 -I $(PYTESTS)/test_devconv.py && cc $(CFLAGS) test_devdef.c ../src/world/devdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_devdef && $(HOST_TMP)/test_devdef $(HOST_TMP)/halo-devs.hp2 $(wildcard ../../assets/hp2/*.dev.hp2)
	cc $(CFLAGS) test_devices.c ../src/sim/devices.c ../src/world/devdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_devices && $(HOST_TMP)/test_devices $(HOST_TMP)/halo-devs.hp2 $(wildcard ../../assets/hp2/*.dev.hp2)
	cc $(CFLAGS) test_vitals.c ../src/sim/vitals.c -lm -o $(HOST_TMP)/test_vitals
	$(HOST_TMP)/test_vitals
	cc $(CFLAGS) test_player.c ../src/sim/player.c ../src/sim/phys.c -lm -o $(HOST_TMP)/test_player
	$(HOST_TMP)/test_player
	cc $(CFLAGS) test_input.c ../src/sim/input.c ../src/core/config.c -o $(HOST_TMP)/test_input
	$(HOST_TMP)/test_input
	cc $(CFLAGS) test_game.c ../src/sim/game.c ../src/sim/save.c ../src/sim/fx.c ../src/ui/hud.c ../src/ui/huddef.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c ../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c ../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_game
	$(HOST_TMP)/test_game ../../assets/hp2
	cc $(CFLAGS) test_save.c ../src/sim/save.c ../src/sim/fx.c ../src/sim/game.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c ../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c ../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_save
	$(HOST_TMP)/test_save
	$(MAKE) -s coll
	$(MAKE) -s cine
	$(MAKE) -s hs
	$(MAKE) -s path
	$(MAKE) -s host_misc
	$(MAKE) -s host_obj
	$(MAKE) -s ai
	$(MAKE) -s units

# World collision (docs/15-coll-section.md): synthetic fixture always; with assets/decompressed/a10.map the converted a10 is
# tested too (brute-force agreement, sweeps, depenetration; the BSPn pack of the same map adds a point-location cross-check)
COLL_REAL = $(and $(wildcard ../../assets/decompressed/a10.map),$(HOST_TMP)/halo-coll-a10.hp2)
coll: $(HOST_TMP)/halo-coll.hp2 $(COLL_REAL)
	cc $(CFLAGS) -O1 test_coll.c ../src/world/bsp.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_coll
	$(HOST_TMP)/test_coll $(HOST_TMP)/halo-coll.hp2 $(COLL_REAL) $(and $(COLL_REAL),$(wildcard ../../assets/hp2/a10.hp2))
	@if [ -n "$(COLL_REAL)" ]; then cc -std=c11 -O2 -Wall -Wextra -Werror -I../src test_coll.c ../src/world/bsp.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_coll_bench && COLL_BENCH=1 $(HOST_TMP)/test_coll_bench $(HOST_TMP)/halo-coll.hp2 $(COLL_REAL) > $(HOST_TMP)/coll-bench.txt && grep bench $(HOST_TMP)/coll-bench.txt; fi

$(HOST_TMP)/halo-coll.hp2: $(PYTESTS)/test_collconv.py ../../tools/halo2ps2/collconv.py ../../tools/halo2ps2/hp2.py
	COLL_FIXTURE=$@ python3 -I $(PYTESTS)/test_collconv.py

$(HOST_TMP)/halo-coll-a10.hp2: ../../assets/decompressed/a10.map ../../tools/halo2ps2/collconv.py
	python3 -I ../../tools/halo2ps2/collconv.py ../../assets/decompressed/a10.map -o $@

# Pathfinding (docs/26-path-section.md): the synthetic fixture always; with assets/decompressed/<map>.map the converted PATn sections are
# tested against the collision pack of the same map (A* against a brute-force Dijkstra, every route leg followed by a sphere through
# coll_sphere_sweep, ...). PATH_MAPS="a10 a30 ..." (or "all") picks the maps; the default is a10 (about 10 s under the sanitizers).
PATH_ALL = a10 a30 a50 b30 b40 c10 c20 c40 d20 d40
PATH_MAPS ?= a10
PATH_REAL_MAPS = $(foreach m,$(if $(filter all,$(PATH_MAPS)),$(PATH_ALL),$(PATH_MAPS)),$(if $(wildcard ../../assets/decompressed/$(m).map),$(m)))
PATH_ARGS = $(foreach m,$(PATH_REAL_MAPS),$(HOST_TMP)/halo-path-$(m).hp2 $(HOST_TMP)/halo-path-coll-$(m).hp2)
path: $(HOST_TMP)/halo-path.hp2 $(PATH_ARGS)
	cc $(CFLAGS) -O1 test_path.c ../src/ai/path.c ../src/world/coll.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_path
	$(HOST_TMP)/test_path $(HOST_TMP)/halo-path.hp2 $(PATH_ARGS)
	@if [ -n "$(PATH_ARGS)" ]; then cc -std=c11 -O2 -Wall -Wextra -Werror -I../src test_path.c ../src/ai/path.c ../src/world/coll.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_path_bench && PATH_BENCH=1 $(HOST_TMP)/test_path_bench $(HOST_TMP)/halo-path.hp2 $(PATH_ARGS) > $(HOST_TMP)/path-bench.txt && grep bench $(HOST_TMP)/path-bench.txt; fi

$(HOST_TMP)/halo-path.hp2: $(PYTESTS)/test_pathconv.py ../../tools/halo2ps2/pathconv.py ../../tools/halo2ps2/hp2.py
	PATH_FIXTURE=$@ python3 -I $(PYTESTS)/test_pathconv.py

$(HOST_TMP)/halo-path-%.hp2: ../../assets/decompressed/%.map ../../tools/halo2ps2/pathconv.py
	python3 -I ../../tools/halo2ps2/pathconv.py $< -o $@ > /dev/null

$(HOST_TMP)/halo-path-coll-%.hp2: ../../assets/decompressed/%.map ../../tools/halo2ps2/collconv.py
	python3 -I ../../tools/halo2ps2/collconv.py $< -o $@ > /dev/null

# Script machine (docs/24-script-section.md): synthetic worlds + the Python-written fixture always; every assets/hp2/<map>.script.hp2
# (python3 -I tools/halo2ps2/scriptconv.py assets/decompressed/<map>.map) is run for 3000 ticks too. The -O2 build prints the timings.
hs: $(PYTESTS)/test_scriptconv.py ../../tools/halo2ps2/scriptconv.py
	HS_FIXTURE=$(HOST_TMP)/halo-script.hp2 python3 -I $(PYTESTS)/test_scriptconv.py
	cc $(CFLAGS) test_hs.c ../src/script/hs.c ../src/script/hs_core.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_hs
	$(HOST_TMP)/test_hs $(HOST_TMP)/halo-script.hp2 $(wildcard ../../assets/hp2/*.script.hp2)
	@if [ -n "$(wildcard ../../assets/hp2/a10.script.hp2)" ]; then cc -std=c11 -O2 -Wall -Wextra -Werror -I../src test_hs.c ../src/script/hs.c ../src/script/hs_core.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_hs_bench && HS_BENCH=1 $(HOST_TMP)/test_hs_bench $(HOST_TMP)/halo-script.hp2 $(wildcard ../../assets/hp2/*.script.hp2) > $(HOST_TMP)/hs-bench.txt && grep -E "a10 |a50 |c20 " $(HOST_TMP)/hs-bench.txt; fi

# Cinematics and camera (docs/28-cine-section.md): the synthetic fixture always; with assets/decompressed/<map>.map every campaign map is converted
# (cinconv.py) and every recording of it is played tick by tick against the Python reference player's per tick hashes ("golden") and fuzzed.
CINE_MAPS = $(filter-out ui,$(notdir $(basename $(wildcard ../../assets/decompressed/*.map))))
CINE_REAL = $(foreach m,$(CINE_MAPS),$(HOST_TMP)/halo-cine-$(m).hp2 $(HOST_TMP)/halo-cine-$(m).golden)
cine: $(CINE_REAL) $(PYTESTS)/test_cinconv.py ../../tools/halo2ps2/cinconv.py
	CINE_FIXTURE=$(HOST_TMP)/halo-cine.hp2 python3 -I $(PYTESTS)/test_cinconv.py
	cc $(CFLAGS) test_cinedef.c ../src/world/cinedef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_cinedef
	$(HOST_TMP)/test_cinedef $(HOST_TMP)/halo-cine.hp2 $(CINE_REAL)
	cc $(CFLAGS) test_cine.c ../src/sim/cine.c ../src/world/cinedef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_cine
	$(HOST_TMP)/test_cine $(HOST_TMP)/halo-cine.hp2 $(CINE_REAL)
	cc $(CFLAGS) test_cine_observer.c ../src/sim/cine.c ../src/world/cinedef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_cine_observer
	$(HOST_TMP)/test_cine_observer

$(HOST_TMP)/halo-cine-%.hp2: ../../assets/decompressed/%.map ../../tools/halo2ps2/cinconv.py
	python3 -I ../../tools/halo2ps2/cinconv.py $< -o $@ > /dev/null

$(HOST_TMP)/halo-cine-%.golden: $(HOST_TMP)/halo-cine-%.hp2
	python3 -I ../../tools/halo2ps2/cinconv.py --golden $< > $@

# Fixture packs are written by the Python tools, so the C reader is checked against the writer.
$(HOST_TMP)/halo-test.hp2: $(PYTESTS)/test_hp2.py ../../tools/halo2ps2/hp2.py
	HP2_FIXTURE=$@ python3 -I $(PYTESTS)/test_hp2.py

$(HOST_TMP)/halo-bsp.hp2: $(PYTESTS)/test_bspconv.py ../../tools/halo2ps2/bspconv.py ../../tools/halo2ps2/hp2.py
	BSP_FIXTURE=$@ python3 -I $(PYTESTS)/test_bspconv.py

# H2 script host (docs/34-host-misc.md): camera / cinematic, sound, hud, navpoints, game flow built-ins on a mission built from the real a10 packs
# (skips without them); needs the CINE pack of a10, converted into HOST_TMP like the cine rule does.
HM_CINE = $(and $(wildcard ../../assets/decompressed/a10.map),$(HOST_TMP)/halo-cine-a10.hp2)
HM_SRC = mission_weak.c ../src/script/hs.c ../src/script/hs_core.c ../src/script/hs_host_misc.c ../src/world/mouth.c ../src/sim/mission.c ../src/sim/cine.c ../src/sim/devices.c ../src/sim/game.c ../src/sim/fx.c \
	../src/ui/hud.c ../src/ui/huddef.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c \
	../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c \
	../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/world/cinedef.c ../src/world/devdef.c ../src/ai/aidef.c \
	../src/audio/strmdef.c ../src/audio/stream.c ../src/audio/snddef.c ../src/stream/hp2.c
host_misc: $(HM_CINE) test_host_misc.c test_host_misc_fixture.h ../src/script/hs_host_misc.c ../src/world/mouth.c
	cc $(CFLAGS) test_host_misc.c $(HM_SRC) -lm -o $(HOST_TMP)/test_host_misc
	$(HOST_TMP)/test_host_misc ../../assets/hp2 $(or $(HM_CINE),$(HOST_TMP)/halo-cine-a10.hp2)

# H1 script host (docs/33-host-objects.md): object / unit / player / device / effect built-ins on the shared mission fixture (mission_fixture.h) and a
# 9000-tick soak of the a10 script machine. ai/units.c (workstream U) is linked as soon as it exists, else the fixture's weak unit layer is used.
HO_SRC = mission_weak.c ../src/script/hs.c ../src/script/hs_core.c ../src/script/hs_host_obj.c ../src/sim/mission.c ../src/sim/cine.c ../src/sim/devices.c ../src/sim/game.c ../src/sim/fx.c \
	../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c \
	../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c \
	../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/world/cinedef.c ../src/world/devdef.c ../src/ai/aidef.c \
	$(wildcard ../src/ai/units.c) ../src/stream/hp2.c
host_obj: $(HM_CINE) test_host_obj.c mission_fixture.h ../src/script/hs_host_obj.c
	cc $(CFLAGS) test_host_obj.c $(HO_SRC) -lm -o $(HOST_TMP)/test_host_obj
	$(HOST_TMP)/test_host_obj ../../assets/hp2 $(or $(HM_CINE),$(HOST_TMP)/halo-cine-a10.hp2)

# AI runtime + ai script built-ins (docs/32-ai-runtime.md): a10 collision / navigation of BSP 0..2 (the PATn pack is converted into HOST_TMP), the shared mission
# fixture, the real unit layer (ai/units.c, workstream U) when it exists. Skips without assets/. The -O2 build prints nothing extra: the test measures itself.
AI_PATH = $(and $(wildcard ../../assets/decompressed/a10.map),$(HOST_TMP)/halo-path-a10-ai.hp2)
AI_SRC = mission_weak.c ../src/ai/ai.c ../src/ai/path.c ../src/script/hs_host_ai.c ../src/script/hs.c ../src/script/hs_core.c ../src/sim/mission.c ../src/sim/cine.c ../src/sim/devices.c \
	../src/sim/game.c ../src/sim/fx.c ../src/ui/hud.c ../src/ui/huddef.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c \
	../src/sim/weapdef.c ../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c \
	../src/world/scen.c ../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/world/cinedef.c ../src/world/devdef.c \
	../src/ai/aidef.c $(wildcard ../src/ai/units.c) ../src/stream/hp2.c ../src/script/hs_host_obj.c ../src/script/hs_host_misc.c ../src/world/mouth.c ../src/audio/strmdef.c ../src/audio/stream.c \
	../src/audio/snddef.c
AI_DEFS = $(if $(wildcard ../src/ai/units.c),-DAI_REAL_UNITS)
ai: $(AI_PATH) test_ai.c mission_fixture.h ../src/ai/ai.c ../src/ai/ai.h ../src/script/hs_host_ai.c
	cc $(CFLAGS) $(AI_DEFS) test_ai.c $(AI_SRC) -lm -o $(HOST_TMP)/test_ai
	$(HOST_TMP)/test_ai ../../assets/hp2 $(or $(AI_PATH),$(HOST_TMP)/halo-path-a10-ai.hp2)

$(HOST_TMP)/halo-path-a10-ai.hp2: ../../assets/decompressed/a10.map ../../tools/halo2ps2/pathconv.py
	python3 -I ../../tools/halo2ps2/pathconv.py $< -o $@ --bsp 0 --bsp 1 --bsp 2 > /dev/null

# Units runtime (docs/31-units.md): locomotion, weapons, animation selection, damage and death of non-player bipeds on the real a10 packs (skips without them), a 96 unit
# tick benchmark and snapshot determinism. The second build (-O2, no sanitizers, UNITS_BENCH=1) enforces the tick budget and prints the timings.
UNITS_SRC = ../src/ai/units.c ../src/sim/game.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c \
	../src/sim/objects.c ../src/sim/skeleton.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c ../src/world/ansm.c \
	../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c
units: test_units.c ../src/ai/units.c ../src/sim/units.h
	cc $(CFLAGS) test_units.c $(UNITS_SRC) -lm -o $(HOST_TMP)/test_units
	$(HOST_TMP)/test_units ../../assets/hp2
	@if [ -n "$(wildcard ../../assets/hp2/a10.obj.hp2)" ]; then cc -std=c11 -O2 -Wall -Wextra -Werror -I../src test_units.c $(UNITS_SRC) -lm -o $(HOST_TMP)/test_units_bench && UNITS_BENCH=1 $(HOST_TMP)/test_units_bench ../../assets/hp2 > $(HOST_TMP)/units-bench.txt && grep perf $(HOST_TMP)/units-bench.txt; fi

.PHONY: test coll hs cine path host_misc host_obj ai units
