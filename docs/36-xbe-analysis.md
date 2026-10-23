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

## The camera (analysis of 2026-10-23, ADR-084)
* `camera_set` of a scenario camera point (0x120eb0): the 0x68 byte point is read at +0x28 position, +0x34 yaw, +0x38 pitch, +0x3c roll, +0x40 field of view (0 = 70 degrees);
  the angles go through `euler_angles3d_to_matrix` (0xb67f0, rows `forward = (c1c2, s1c3 - s2s3c1, s1s3 + s2c3c1)`, `left = (-s1c2, c1c3 + s2s3s1, c1s3 - s2c3s1)`, `up = (-s2, -c2s3, c2c3)`
  for yaw `1`, pitch `2`, roll `3`) and the director keeps position, forward (row 0) and up (row 2). The seconds are `ticks / 30` by integer division. The call ends with an observer update of 1e-4 s.
* The observer (0x11dd30 per player: 0x11c760 copies the director's record, 0x11d2d0 runs 0x11cb10 / 0x11c500 / 0x11c330 / 0x11c130 / 0x11cda0, 0x11d900 adds the focus / distance and the
  collision) has the channels position 3, focus 3, distance 1, field of view 1 and orientation (forward + up as the state, a rotation vector as the offset). Every update refits a quintic
  `e(t) = c3 t^3 + c4 t^4 + c5 t^5` of the offset in the remaining time with the velocity and acceleration of the previous fit (kept analytically in 0x2715c4 / 0x2715fc) and moves the state by
  `D + e(t - dt)`; the orientation channel rotates forward and up about the rotation vector (0x11ca50). A fit whose acceleration exceeds 1500 (positions) or 1e5 snaps the camera.
* The projection: 0xbc260 fills the view from the observer's output: position, forward, up and `vfov = 2 atan(0.6375 tan(fov / 2))` (qword 1.0 at 0x1f0a70 as the divisor of `fpatan`). The view window
  (`FUN_000bba00`) is the title-safe rectangle top 36, left 48, bottom 444, right 592 (544 x 408) of the 640 x 480 screen (set at 0x1f8c5c..0x1f8c6a at start up). 0x5c5e0 maps y by `1 / tan(vfov / 2)` and x
  by `height / (width tan(vfov / 2))`: the horizontal half tangent is `0.85 tan(fov / 2)`, 62 degrees for 70. The cinematic bars are overlay quads (0x29fd0), not a smaller window.

## Not found / still open
* The vertex shader's lighting model for objects (ambient + lights), the exact `V0` for BSP surfaces, the glow / bloom of additive materials, the lens-flare and sun-glow
  passes (`rasterizer_sun_glow_*` exist), the second pass of the model shader for the Cortana hologram's multi-stage `sotr` programs.
* Colour change did not need the executable: the tag data decides it (`chgconv.py`: object tag +0x164, shader_model +0x4C source, multipurpose +0xBC blue channel = mask), ADR-065.
