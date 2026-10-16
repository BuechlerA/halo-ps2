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

# Fixture packs are written by the Python tools, so the C reader is checked against the writer.
$(HOST_TMP)/halo-test.hp2: $(PYTESTS)/test_hp2.py ../../tools/halo2ps2/hp2.py
	HP2_FIXTURE=$@ python3 -I $(PYTESTS)/test_hp2.py

$(HOST_TMP)/halo-bsp.hp2: $(PYTESTS)/test_bspconv.py ../../tools/halo2ps2/bspconv.py ../../tools/halo2ps2/hp2.py
	BSP_FIXTURE=$@ python3 -I $(PYTESTS)/test_bspconv.py

.PHONY: test
