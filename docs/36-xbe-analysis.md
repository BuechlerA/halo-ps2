# What `default.xbe` tells us (analysis of 2026-10-18)

Method: `tools/xbe/` (README there): the XBE becomes a flat image, Ghidra (headless) creates 6000 functions from call targets and code pointers and
decompiles them, a small decoder prints the NV2A register-combiner programs. Nothing here is copied code; it is behaviour, recorded so the
converters and the engine can follow it.

## Facts established
* Layout: `.text` 0x11000..0x180e00 (Halo), the statically linked D3D library 0x180e40.., Bink, DSound. Entry address is XOR-encoded in the header.
* The script built-ins are a table in `.rdata` (name, return type, evaluate function, help text, parameter types): `cinematic_screen_effect_*` evaluate at
  0xee100 (video), 0xee150 (desaturation tint), 0xee1b0 (filter), 0xee210 (convolution), 0xee270 (start); the state is a 0x78-byte record behind the pointer at
  0x2e3504: `+0x14..0x1c` desaturation tint rgb, `+0x38` active, `+0x39` clear request, `+0x3c..0x60` filter / convolution / video fields with start / end
  times (`0x61990` video: noise intensity and overbright 0 / 1x / 2x / 4x, `0x61c40` filter, `0x61cb0` convolution). Halo's own implementation is a screen-space
  pass over a copy of the frame buffer; a10 uses it only for the purple flash that ends the bridge (`fade_out 0.8 0 1`, tint `0.8 0 1`).
* Halo does **not** use D3D pixel shader objects: no call to `D3DDevice_SetPixelShader` (0x184070) exists in Halo code. The rasterizer fills one definition buffer
  (`0x2fce60`, the 0xE4-byte `D3DPIXELSHADERDEF`) in code and installs it with `0x7e5d0`; ~40 functions (0x64000..0x7e000) do so, the shader_model / shader_environment /
  transparent / screen-effect passes among them (the 37..58 KB functions 0x66510, 0x688a0, 0x68f10 are `switch`es over shader types). Texture stage states
  (`0x182150`, 512 call sites) and render states (`0x1820a0`, 370) are the fixed-function remainder.
* Decoded programs (`tools/xbe/decode.py`; T0..T3 textures, V0 diffuse vertex colour, C0 / C1 constants; "x2" = shift left 1):
  - base x detail x2 x (T2 + V0): `R0 = T0*T1*2` (alpha `T0.a*T1.a`), `R1 = T2 + V0`, `out = R0 * R1`, alpha `R0.a`: the environment diffuse pass; the third texture is not
    scaled and is **added** to the vertex colour before it modulates, i.e. the lightmap is not doubled (a lit lightmap texel of 249 is about 1.0 of the base colour, which
    is what the tone curve of ADR-064 ends at) and a vertex term lifts the shadows (the curve does too). What V0 carries for a10 is not established: the sky tag's indoor
    ambient power is 0.
  - texture x vertex colour (`R0 = T0*V0`): objects without detail.
  - 2-stage lerp programs `(1-T1.b)*C1.a + T1.b*C0.a` (a mask in the blue channel selects between two constants), reflection / specular `dotRS`, bump `dotRD` and
    `cube` stages for the environment's specular and reflection passes, and four-texture normal-map programs.
  The full per-pass assignment of textures to stages was not completed: the programs are parametric (stores through registers), which `defs.py` handles only
  partly.

## Not found / still open
* The vertex shader's lighting model for objects (ambient + lights), the exact `V0` for BSP surfaces, the glow / bloom of additive materials, the lens-flare and sun-glow
  passes (`rasterizer_sun_glow_*` exist), the second pass of the model shader for the Cortana hologram's multi-stage `sotr` programs.
* Colour change did not need the executable: the tag data decides it (`chgconv.py`: object tag +0x164, shader_model +0x4C source, multipurpose +0xBC blue channel = mask), ADR-065.
