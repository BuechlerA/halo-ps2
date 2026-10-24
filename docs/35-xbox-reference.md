# Xbox reference emulator (xemu) for parity checks

The port is built and gated on PCSX2 (ADR-015). To compare what the PS2 draws with what the original game draws at the same
camera position (ADR-055), the user's own Halo ISO is booted in **xemu**, the original Xbox emulator. This is a development aid only: no
Xbox data leaves `assets/` or the ISO, and nothing here is part of the engine.

## What you need to supply
xemu will not start without Microsoft's boot files; they are not in the repository and the setup script never downloads them.
Dump them from your own console (or use files you already own) and put them in one folder, default `~/Developer/xbox-bios/` (files may sit one subfolder down, e.g. `mcpx/`, `bios/`, `hdd/`):

| File | What | Check |
|---|---|---|
| `mcpx_1.0.bin` (any `mcpx*.bin`) | MCPX boot ROM | exactly 512 bytes |
| `Complex_4627.bin` (any other `*.bin`) | flash BIOS | 256 KiB, 512 KiB or 1 MiB |
| `xbox_hdd.qcow2` (any `*.qcow2`) | formatted HDD image (xemu's getting-started page links a blank formatted one) | readable by `qemu-img info` |

The MCPX 1.0 ROM works with the 4627-era retail BIOSes; Halo (build 01.10.12.2276) runs on those.

## Setup and run
```bash
brew install --cask xemu                # done on this machine: /Applications/xemu.app, 0.8.137
scripts/xemu-setup.sh                   # validates the three files, writes assets/xemu/xemu.toml (gitignored)
scripts/xemu-run.sh                     # boots halo.xiso.iso from the main checkout
```
Overrides: `XBOX_BIOS_DIR`, `XBOX_MCPX`, `XBOX_FLASH`, `XBOX_HDD` (setup), `HALO_XISO` or a path argument (run), `XEMU` (both).
The config is project-local (`-config_path`), so your global xemu settings stay untouched; the EEPROM is created beside it on first boot.
Each git worktree has its own `assets/`, so rerun setup in a new worktree (it only rewrites the config).

## Status
Booted on 2026-10-17 (xemu 0.8.137, M4 Max, MCPX 1.0 + Complex 4627 BIOS): the Halo ISO reaches the Bungie logo. Not yet checked: gameplay,
controller mapping, screenshots of a mission. A config without `[general] show_welcome = false` made xemu start without `-bios`
(log: `Failed to load BIOS '(null)'`); the generated config sets it, cause beyond that is not investigated. xemu's log is the quickest
check: `Created QEMU launch parameters` must contain `-bios`.

## Using it for parity (next steps, not done)
- Same-camera comparison: Halo's debug camera and `cheat_*` console are not available on a retail build, so the practical route is
  matching the camera in the engine to a screenshot taken in xemu (`scripts/campos.py` already derives positions from the Xbox data).
- xemu's `Machine > Take Screenshot` gives lossless PNGs; `scripts/refrender.py` stays the independent reference renderer for CPU-side checks.
- Frame times from xemu are not Xbox timings; use it for picture parity only.

## What xemu cannot be used for (2026-10-23)
`xemu` accepts `-qmp tcp:127.0.0.1:4444,server=on,wait=off` (QMP: `send-key` with qcodes works, `pmemsave` dumps guest RAM, there is no `screendump`; the window is captured with
`screencapture -l <id>`), but the main menu did not take the A button reliably (the attract loop restarted instead of the campaign) and the intro is 4 minutes in. Do not spend time driving it: the
executable itself is the ground truth, read with `tools/xbe` (docs/36) and run under an emulator (`tools/xbe/observer_oracle.py`). The reference video is an approximation (it is not necessarily a
4:3 Xbox capture): where it disagrees with the executable, the executable wins (ADR-084).
