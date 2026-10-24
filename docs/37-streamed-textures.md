# Streamed high resolution textures (objects and the BSP)

Resident textures are half the size of their sources (a 512 x 512 map is 256 x 256, a 1024 x 256 screen 512 x 128) because the arena and the 6 MB GS texture pool have no
room for more. Close up they blur: the bridge's consoles and wall displays in the opening cutscene were the case that showed it. Two companion packs hold the sharper versions
and stream them in on demand, one system for both:

| pack | next to | holds | indexed by | renderer |
|---|---|---|---|---|
| `<map>.hires.hp2` | `<map>.obj.hp2` | variants of object textures (256 per side, 512 hero textures) | object texture index | `render/obj_render.c` (ADR-074) |
| `<map>.thires.hp2` | `<map>.tex.hp2` | variants of BSP textures at the source's size (up to 1024 per side) | BSP texture index (`TEXS`) | `render/bsp_render.c` (ADR-082) |

Both are written by the converters (`objpack.py`, `texconv.py`), stored uncompressed, and have the same layout: a `TEXS` section of textures only (one mip level, no bindings; a
variant's CLUT and pixels are one contiguous range) and `HMAP` (u32 count of base textures, u16 per base texture: its variant's index or `0xFFFF`). A variant must fit one staging
slot: 128 KB of pixels (`HIRES_MAX_BYTES`), so a 512 x 512 or 1024 x 256 source becomes a 16 colour `PSMT4` (RMS error is not checked: the palette decides for 8 bits first,
16 colours only when the pixels do not fit). a10: 169 object variants (6.9 MiB), 21 BSP variants (1.9 MiB: tech rack mount 512 x 512, monitors 1024 x 256, panel strips 1024 x 256, panels, vending machines).
The format limit for a texture side is 1024 (`TEX_MAX_DIM`); resident textures never exceed 512.

## Runtime (`world/hires.[ch]`, `stream/aread.[ch]`)
* **One ring, several sets.** `hires_ring_t` holds `hiresslots` (default 3) staging slots of the largest variant (129 KB on a10) in the arena above the BSP mark; every set (`hires_t`,
  tables only, a few KB) borrows it. The ring is dropped when less than 1 MB of the arena is left after it (b30).
* **Asynchronous.** `hires_request(set, base, &view)` returns 0 with the variant's CLUT / pixels when it is in a slot; otherwise it starts a read (`aread`: an IO thread making blocking
  fio calls, the same pattern as `audio/stream.c`; one read in flight for the whole ring, at most `hiresloads` (1) started per frame) and returns -1, and the caller draws the resident
  texture until a later frame finds the variant. Before this the read was a blocking `fioRead` on the game thread: 129 KB per variant, the 38 ms frames at ticks 1175 / 1221 / 1265 of
  the intro (real speed, 5 of the 12 frames the bridge approach dropped), gone now (peak EE prep 39 -> 25 ms).
* **Slot lifetime.** FREE -> FLIGHT -> READY -> (requested) HOLD -> FREE. A slot on HOLD stays untouched for `HIRES_HOLD` (3) frames because the GS DMA reads it at the end of the frame and
  the chain may run a frame later; a READY variant nobody asks for within `HIRES_READY_TTL` (12) frames is dropped (the object / surface left the view); when every slot is READY or on
  HOLD the oldest unclaimed READY one is recycled. The GS texture cache (`bsp_render.c`, keyed by a reserved range per set) remains the only LRU: a variant that is resident there is
  drawn without touching the ring.
* **Objects** ask when their on-screen diameter reaches `hirespx` (200 px on the 640 scale; from half of that the read starts).
* **BSP materials** ask by texel density. The converter stores `uv_world` per material (BSP material record +0x10, f32: world units per uv unit = sqrt(world area / uv area), 0 = unknown,
  `bspconv.uv_world`). Per frame and visible material the renderer takes the nearest visible batch (distance to its bounding sphere, at least 0.05), computes screen pixels per world unit
  (`256 / tan(hfov / 2) / d`) and texels per world unit (`sqrt(w h su sv) / uv_world` of the *resident* texture with the shader's tiling), and asks for the variant when pixels per texel
  reach `bsphiresratio` (0.7: the resident copy starts to blur) and starts the read from half of that. The test needs no per-map tuning, so gameplay maps get it from the same data:
  floors and walls the player stands next to load their variants, far surfaces never do.
* A variant whose upload fails (no room in the GS cache) is not requested again for 90 frames.

## Config keys
`hires 0|1` (object set), `bsphires 0|1` (BSP set), `hiresslots` (3), `hiresloads` (1), `hirespx` (200), `bsphiresratio` (0.7). End of run: `[hires] N loads, KB read, refused, failed, unclaimed`.

## Measured
a10 intro, tick 2250 (the bridge console, camera 0.3 units from the panel): the labels, the hex mesh of the display and the key legends resolve; the resident texture was a blur. Real speed,
ticks 0 .. 1500: 25 loads / 1.8 MB (23 loads / 1.4 MB sampling the whole intro), 0 failed, no frame over budget except the BSP switch. GS cost of the 128 KB uploads is not measured (PCSX2
does not model it, ADR-022); a first close-up needs a few frames of disc reads (tens of ms each on a real disc, spread over frames by the budget).

## Not done
Streaming the lightmap pages, the glow pass's texture (`texture2`, drawn at the resident size) and sky bitmaps; per-surface (not per-material) density; a prefetch hook for cuts; bump-baked hull
textures (`poa-*`) have no variant.
