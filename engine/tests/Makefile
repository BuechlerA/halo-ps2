# Host-compiled unit tests for the platform-independent engine code. Needs only python3 and a
# C compiler (no ps2dev), so CI runs it on a stock runner:  make -C engine/tests
HOST_TMP ?= $(or $(TMPDIR),/tmp)
CFLAGS   ?= -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I../src
PYTESTS  = ../../tools/tests

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
	FXDEF_FIXTURE=$(HOST_TMP)/halo-fxdef.hp2 python3 -I $(PYTESTS)/test_fxconv.py && cc $(CFLAGS) test_fxdef.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_fxdef && $(HOST_TMP)/test_fxdef $(HOST_TMP)/halo-fxdef.hp2
	ANSM_FIXTURE=$(HOST_TMP)/halo-ansm.hp2 python3 -I $(PYTESTS)/test_ansmconv.py && cc $(CFLAGS) test_ansm.c ../src/world/ansm.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_ansm && $(HOST_TMP)/test_ansm $(HOST_TMP)/halo-ansm.hp2
	UNIT_FIXTURE=$(HOST_TMP)/halo-unit.hp2 python3 -I $(PYTESTS)/test_unitconv.py && cc $(CFLAGS) test_unitdef.c ../src/sim/unitdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_unitdef && $(HOST_TMP)/test_unitdef $(HOST_TMP)/halo-unit.hp2 $(wildcard ../../assets/hp2/a10.unit.hp2)
	CMOD_FIXTURE=$(HOST_TMP)/halo-cmod.hp2 python3 -I $(PYTESTS)/test_cmodconv.py && cc $(CFLAGS) test_cmod.c ../src/world/cmod.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_cmod && $(HOST_TMP)/test_cmod $(HOST_TMP)/halo-cmod.hp2
	SND_FIXTURE=$(HOST_TMP)/halo-snd.hp2 python3 -I $(PYTESTS)/test_soundconv.py && cc $(CFLAGS) test_snddef.c ../src/audio/snddef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_snddef && $(HOST_TMP)/test_snddef $(HOST_TMP)/halo-snd.hp2 $(wildcard ../../assets/hp2/*.snd.hp2)
	WEAP_FIXTURE=$(HOST_TMP)/halo-weap.hp2 python3 -I $(PYTESTS)/test_weapconv.py && cc $(CFLAGS) test_weapdef.c ../src/sim/weapdef.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_weapdef && $(HOST_TMP)/test_weapdef $(HOST_TMP)/halo-weap.hp2 $(wildcard ../../assets/hp2/*.weap.hp2)
	HUD_FIXTURE=$(HOST_TMP)/halo-hud.hp2 python3 -I $(PYTESTS)/test_hudconv.py && cc $(CFLAGS) test_huddef.c ../src/ui/huddef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_huddef && $(HOST_TMP)/test_huddef $(HOST_TMP)/halo-hud.hp2
	cc $(CFLAGS) test_vitals.c ../src/sim/vitals.c -lm -o $(HOST_TMP)/test_vitals
	$(HOST_TMP)/test_vitals
	cc $(CFLAGS) test_player.c ../src/sim/player.c ../src/sim/phys.c -lm -o $(HOST_TMP)/test_player
	$(HOST_TMP)/test_player
	cc $(CFLAGS) test_input.c ../src/sim/input.c ../src/core/config.c -o $(HOST_TMP)/test_input
	$(HOST_TMP)/test_input
	cc $(CFLAGS) test_game.c ../src/sim/game.c ../src/sim/save.c ../src/sim/fx.c ../src/ui/hud.c ../src/ui/huddef.c ../src/sim/playerdef.c ../src/sim/player.c ../src/sim/phys.c ../src/sim/vitals.c ../src/sim/unitdef.c ../src/sim/weapdef.c ../src/sim/objects.c ../src/sim/skeleton.c ../src/sim/fpview.c ../src/core/mat.c ../src/render/camera.c ../src/world/anim.c ../src/world/model.c ../src/world/scen.c ../src/world/ansm.c ../src/world/cmod.c ../src/world/coll.c ../src/world/fxdef.c ../src/world/tex.c ../src/stream/hp2.c -lm -o $(HOST_TMP)/test_game
	$(HOST_TMP)/test_game ../../assets/hp2
	$(MAKE) -s coll

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

# Fixture packs are written by the Python tools, so the C reader is checked against the writer.
$(HOST_TMP)/halo-test.hp2: $(PYTESTS)/test_hp2.py ../../tools/halo2ps2/hp2.py
	HP2_FIXTURE=$@ python3 -I $(PYTESTS)/test_hp2.py

$(HOST_TMP)/halo-bsp.hp2: $(PYTESTS)/test_bspconv.py ../../tools/halo2ps2/bspconv.py ../../tools/halo2ps2/hp2.py
	BSP_FIXTURE=$@ python3 -I $(PYTESTS)/test_bspconv.py

.PHONY: test coll
