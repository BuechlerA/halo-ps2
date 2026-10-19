# tools/xbe: reading the Xbox executable (docs/36-xbe-analysis.md)

Needs the user's own `assets/xbox/default.xbe` (never committed), Ghidra 12 (`analyzeHeadless`), `ndisasm`, Python 3. All scripts expect to run
inside a scratch directory that holds `halo_flat.bin`, and write there.

1. `python3 -I xbe2flat.py assets/xbox/default.xbe halo_flat.bin`
2. `analyzeHeadless proj halo -import halo_flat.bin -processor x86:LE:32:default -loader BinaryLoader -loader-baseAddr 0x10000 -noanalysis -scriptPath <dir with Dump.java> -postScript Dump.java`
   creates a function at every `call rel32` target and every 16-aligned code pointer in the data sections and writes `halo_dec.c` (6000 decompiled functions, a minute).
3. `fn.py` (`fn.py 00184070`, `fn.py c 0007e5d0` = callers) reads `halo_dec.c`; `dis.py START LEN` disassembles; `defs.py` / `decode.py` rebuild the
   pixel-shader programs that Halo's rasterizer assembles at runtime (buffer `0x2fce60`, installed through `0x7e5d0`) and print them as combiner equations.
