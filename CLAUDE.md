# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Project Does

LED controller for UGREEN DX/DXP/iDX NAS series. The NAS LED chip (Holtek HT32F52231 MCU) is accessible via I²C at address `0x3a` on the SMBus I801 adapter. This project provides two independent ways to control the LEDs:

1. **CLI tool** (`cli/`) — userspace C++ binary that talks to the I²C device directly. Requires root and `i2c-dev` module. **Conflicts with the kernel module** — unload `led_ugreen` before using.
2. **Kernel module** (`kmod/`) — exposes LEDs under `/sys/class/leds/` using the Linux LED subsystem. Standard `ledtrig-netdev` and `ledtrig-oneshot` triggers then drive them.

The `scripts/` directory contains bash daemons (`ugreen-diskiomon`, `ugreen-netdevmon`, `ugreen-power-led`) that sit on top of the kernel module and implement disk activity blinking, network activity blinking, and standby color changes. Optional C++ helpers (`scripts/blink-disk.cpp`, `scripts/check-standby.cpp`) reduce CPU usage and latency for those daemons.

## Build Commands

**CLI tool** (requires `g++`, produces a static binary):
```bash
cd cli && make
# binary: cli/ugreen_leds_cli
```

**Kernel module** (requires kernel headers for the running kernel):
```bash
cd kmod && make
sudo insmod kmod/led-ugreen.ko
```

**Optional C++ helpers** (run from `scripts/`):
```bash
g++ -std=c++17 -O2 blink-disk.cpp -o ugreen-blink-disk
g++ -std=c++17 -O2 check-standby.cpp -o ugreen-check-standby
```

There are no tests and no linter configured.

## Model Detection and the Two Protocol Families

The codebase handles two hardware variants with different I²C wire protocols:

| Series | I²C transfer | LED register mapping |
|--------|-------------|----------------------|
| DX/DXP | `i2c_smbus_write_i2c_block_data` (I2C block write, no count byte) | power=0, netdev=1, disk1–8 = 2–9 |
| iDX6011/iDX6012 | `i2c_smbus_write_block_data` (SMBus block write, kernel adds count byte) | power=0, netdev=1, netdev2=2, disk1–6 = 3–8 |

Model detection (`ugreen_leds_t::detect_model()` in `cli/ugreen_leds.cpp`, and equivalent in `kmod/led-ugreen.c`) reads `/sys/class/dmi/id/product_name`. In LXC containers where DMI is unavailable, set `UGREEN_MODEL=idx6011` (or `dxp`) as an environment variable.

The iDX6011 Pro also requires a **5-step init sequence** on first use to take the MCU out of its autonomous animation mode (`ugreen_leds_t::init_idx6011()`). `needs_init_idx6011()` probes register `0x80` to check whether init is needed before sending it.

## LED Naming vs. I²C Register Values

`led_type_t` enum values (`power`, `netdev`, `netdev2`, `disk1`…`disk8`) are **not** I²C register numbers. Always call `get_i2c_reg(id)` to convert. `get_i2c_reg()` returns `0xff` for unsupported LEDs on the current model (e.g. `netdev2` on DXP, `disk7/8` on iDX6011).

Status is read from register `0x81 + i2c_reg`; writes go to the `i2c_reg` itself.

## Configuration (`/etc/ugreen-leds.conf`)

The shell scripts source `/etc/ugreen-leds.conf` (see `scripts/ugreen-leds.conf` for all options). Key options:

- `MAPPING_METHOD` — `ata` (default), `hctl`, or `serial` — controls how `ugreen-diskiomon` maps physical disk slots to disk LEDs. The iDX6011 Pro ata/hctl mapping is **unverified**; use `serial` as a safe fallback.
- `BLINK_MON_PATH` / `STANDBY_MON_PATH` — paths to the optional compiled helpers.
- Color and brightness variables for each LED state (health, standby, unavailable, etc.).

## Disk Mapping Gotcha

`/dev/sdX` names change at boot. The three stable mapping methods, in order of robustness: serial > ata > hctl. For DXP6800 Pro the ata/hctl order differs from other models — the script detects this with `dmidecode`. For iDX6011 Pro, the ata/hctl order is not yet confirmed; a warning is printed at runtime.

Run `scripts/ugreen-detect-disks` to enumerate the current mapping before relying on it.
