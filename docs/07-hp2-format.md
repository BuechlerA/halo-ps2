# `.hp2` pack format, version 1

One pack per mission (see [02-specs.md](02-specs.md) §6, [03-engine-architecture.md](03-engine-architecture.md) §7).
All integers are little-endian. Offsets are file offsets. Section payloads start on
**2048-byte** (DVD sector) boundaries so the IOP can read them with no bounce copy;
inside a section everything is 16-byte aligned by the converter.

## Header (64 bytes)
| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | magic `HP2P` |
| 0x04 | u16 | version (1) |
| 0x06 | u16 | section count (≤ 64) |
| 0x08 | u32 | total file size |
| 0x0C | u32 | flags (0) |
| 0x10 | char[16] | pack name, NUL padded (the map name, e.g. `a10`) |
| 0x20 | u8[32] | reserved, zero |

## Section table (32 bytes per entry, immediately after the header)
| Offset | Type | Field |
|---|---|---|
| 0x00 | char[4] | section tag: `RES\0`, `BSP0`..`BSP9`, `TEX\0`, `SND\0`, `STR\0` |
| 0x04 | u32 | file offset of the stored payload (multiple of 2048) |
| 0x08 | u32 | stored size in bytes |
| 0x0C | u32 | raw (decompressed) size in bytes |
| 0x10 | u32 | codec: 0 = stored, 1 = LZ4 block |
| 0x14 | u32 | CRC-32 (zlib polynomial) of the **raw** payload |
| 0x18 | u32 | alignment the loader must give the raw buffer (16, 64 or 128) |
| 0x1C | u32 | reserved, zero |

Tags are unique within a pack. The loader finds a section by tag, reads
`stored_size` bytes at `offset`, and decodes into a buffer of `raw_size` bytes.

## Codec
ADR-012 keeps LZ4 as the shipping codec until a custom EE codec beats it on
hardware, so codec 1 is the **standard LZ4 block format** (no frame header); any
LZ4 tool can inspect a payload. A custom codec would take a new codec id.
The reference encoder (`tools/halo2ps2/hp2.py`) is a greedy hash-chain-free
matcher; ratio is not tuned yet. The reference decoder
(`engine/src/stream/hp2.c`) is bounds-checked on both source and destination and
needs only byte loads, so it is correct on the EE but not yet fast; the 128-bit
`lq`/`sq` copy path is future work gated on ADR-012's measurement.

## Verification
`python3 -I tools/tests/test_hp2.py` round-trips packs through the Python codec.
`make -C engine test` builds the C reader with the host compiler and decodes a
pack written by the Python tool, so both implementations are checked against
each other on every CI run.
