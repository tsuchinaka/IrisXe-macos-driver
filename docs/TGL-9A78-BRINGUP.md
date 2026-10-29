# Tiger Lake UHD 48 EU (`8086:9A78`) bring-up

Status: experimental branch `tgl-9a78-support`.

Target machine: Intel Core i3-1115G4, **UHD Graphics 48 EU** (`8086:9A78`), 1366x768 eDP, OpenCore.
Reference machine for the driver remains the 9A49 / Iris Xe 80 EU / 2560x1600 laptop.

## Why this is a small port, not a new driver

- Both parts are **Tiger Lake GT2**; the display engine, GuC/HuC/DMC firmware and media blocks are the same.
- The pinned `drm-kmod` already lists `0x9A78` in `INTEL_TGL_GT2_IDS`.
- `kexts/NMIntelFB/display_ids.inc` already maps `0x9a78` to display version 12.
- `NMXGPU::probe()` accepts any Intel display-class device with `nmxintel=1`; there is no device allow-list.
- EU count is read from fuse registers by i915/ANV. No source file assumes 80 EU or 48 EU.

The only hard-coded part-specific value was the clock-governor ceiling (1300 MHz). UHD 48 EU tops out at
**1250 MHz**, so the branch clamps the governor to the range i915 reports and keeps 1300 for 9A49.

## Build

1. Fork this repository and run **Actions -> "NMXGPU Intel (i915) kext build"** with:
   - `min_os: 15.0` for Sequoia (tested reference OS), or `14.0` for Sonoma.
2. Run **Actions -> "Mesa ANV + Metal plugin"** once for the full `NVMTLDriver.bundle` (with `air-runtime/`).
3. For Sequoia use `out/NMXAccel.kext`; `out/tahoe/NMXAccel.kext` is for macOS 26 only.

## OpenCore profile (driver USB)

Keep this on a **separate USB EFI**; the internal EFI stays untouched.

| Key | Value |
|---|---|
| `Kernel > Add` | `NMXGPU.kext` from EFI, `MinKernel 24.0.0`, `MaxKernel 24.99.99` (Sequoia) |
| `Kernel > Block` | `com.apple.iokit.IONDRVSupport`, `Exclude`, same kernel range |
| WhateverXe / WhateverGreen | **disabled/removed** |
| `boot-args` | `keepsyms=1 debug=0x100 -vi2c-force-polling alcid=55 nmxintel=1 nmxaccel=1 -nmintelfboff nmxws=1 nmxcopyfwd=1 nmxautoarm=1 nmxmodern=1 nmxintelfb=1 -nmxnosnap` |
| `DeviceProperties` `PciRoot(0x0)/Pci(0x2,0x0)` | real `device-id 789A0000`, `AAPL,slot-name built-in`; do **not** copy the 9A49 `499A0000` values |
| `csr-active-config` | `03080000` (0x803) |
| `SecureBootModel` | `Disabled` |

After any config edit: regenerate `vault.plist` and run `ocvalidate` 1.0.8. Do not block `IONDRVSupport` on the
installer EFI; only on the post-install driver EFI.

## Bring-up ladder

### 1. Headless attach

Add `nmxnofb=1` to `boot-args` and boot the driver USB.

Pass conditions:

- log: `NullMoth GPU: driving 8086:9a78`
- log: `nmxgpumhz=550 -> GPU clock floor ... (part 100..1250 MHz)`
- `tools/drmprobe/drmprobe` sees `i915`, chipset `0x9a78`, and reports the EU total as 48
- `vkprobe` creates a Vulkan device through Mesa ANV

No panel takeover yet. If this fails, stop here and collect the log; nothing above the display layer is
board-specific.

### 2. Panel takeover

Remove `nmxnofb=1`, keep `nmxintelfb=1`.

Check:

- 1366x768 eDP mode, no flicker
- brightness slider and keys (`SSDT-PNLF` present)
- display sleep / wake, PSR state
- `tools/macos-helpers/nmx-display.sh` output for eDP/DDI routing

Fallbacks: `-nmxnopsr`, `-nmxnoext`, then collect `ioreg -lw0 -c NMXGPUFB` and the NullMoth log.

Backlight note: `nmx_fb.cpp` drives the PCH PWM registers (`0xC8250..8`). If the panel uses LPSS PWM instead,
brightness will not move; that requires an additional backlight path keyed from the VBT panel type.

### 3. Acceleration

Keep `nmxws=1 nmxautoarm=1`, install `NMXAccel`, plugin, translator and autoarm via IrisXe Manager.

Check:

- `system_profiler SPDisplaysDataType` -> UHD Graphics, Metal, 1536 MB
- `/Library/GPUBundles/nvmtl-allow.txt` includes WindowServer and the apps you test
- `debug.nvaccelfb = 1`
- `nmx-boottime.sh` / `nmx-speed.sh` sane

### 4. Stability

- 1 hour idle, sleep/wake cycles
- external HDMI (mirror only)
- no boot-guard trip (`nullmoth-gpu-attaching`)

## Escape hatches

- `-nvoff` parks the driver for one boot
- `sudo nvram -d nullmoth-gpu-attaching` clears the attach guard
- `sudo nvram -d nmx-autoarm-step` clears the acceleration lock
- boot without the driver USB to get a plain macOS
- IrisXe Manager keeps backups in `/Library/Application Support/IrisXe/backup/`

## Performance expectations

48 EU is not an 80 EU part. Do not compare Metal scores with the 9A49 reference; the goal is correct display,
correct power states and a usable accelerated desktop. The governor now caps at the part's real maximum
(1250 MHz) instead of requesting 1300.
