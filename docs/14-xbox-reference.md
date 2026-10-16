# Xbox reference emulator (xemu) for parity checks

The port is built and gated on PCSX2 (ADR-015). To compare what the PS2 draws with what the original game draws at the same
camera position (ADR-030), the user's own Halo ISO is booted in **xemu**, the original Xbox emulator. This is a development aid only: no
Xbox data leaves `assets/` or the ISO, and nothing here is part of the engine.

## What you need to supply
xemu will not start without Microsoft's boot files; they are not in the repository and the setup script never downloads them.
Dump them from your own console (or use files you already own) and put them in one folder, default `~/Developer/xbox-bios/`:

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
Scripts are tested only against stand-in files (error paths, size checks, config generation, argument assembly). **Not yet booted:**
the real boot files were not on the machine when this was written, so the first real boot, controller mapping and the xemu
`[sys.files]` key names should be confirmed once the files are in place.

## Using it for parity (next steps, not done)
- Same-camera comparison: Halo's debug camera and `cheat_*` console are not available on a retail build, so the practical route is
  matching the camera in the engine to a screenshot taken in xemu (`scripts/campos.py` already derives positions from the Xbox data).
- xemu's `Machine > Take Screenshot` gives lossless PNGs; `scripts/refrender.py` stays the independent reference renderer for CPU-side checks.
- Frame times from xemu are not Xbox timings; use it for picture parity only.
