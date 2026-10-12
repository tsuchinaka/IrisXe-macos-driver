# Intel Iris Xe graphics driver for macOS 14 Sonoma (experimental)

An experimental port of Intel's Linux/FreeBSD **i915** graphics driver to macOS 14 Sonoma on x86_64 (Hackintosh /
OpenCore), aimed at 11th-gen **Iris Xe (Tiger Lake, `8086:9A49`)**. The goal is a kernel driver, a Vulkan driver and
a Metal device for the Intel GPU, where macOS has no native support for it.

This repository is a fork of [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver). All of the
GPU-port design and most of the code is NullMoth Systems' work: the NVIDIA Metal driver, plus the AMD/Intel kernel port
in `gpu-port/` that this fork builds on. This fork's contribution so far is getting the Intel (i915) path to build for
Sonoma and documenting it. The original NVIDIA README is kept at [`docs/README-NVIDIA.md`](docs/README-NVIDIA.md).

> **Status: it builds. It has never been loaded on hardware.**
> Nothing here has been run on a real Iris Xe machine yet. Loading a GPU driver on the GPU that drives your screen can
> black out the display or panic the kernel. Read [Safety](#safety) before you try it, and do not use a machine you
> cannot recover.

## Where it stands

| Stage | State |
|---|---|
| Compiles on macOS 14 runner, Xcode 15.4, SDK 14.5 (x86_64) | **Done.** 900 core + 274 i915 + 75 runtime sources, 0 failures |
| Links `NMXGPU.kext` (13.4 MB, Mach-O x86_64 kext bundle) | **Done.** 0 internally-missing symbols; 544 undefined, all kernel KPI |
| Symbols resolve against Sonoma's real kernel collection | Not checked |
| Kext loads on Sonoma 14.8.9, all symbols resolve, firmware found, boot guard works | **Done** (hardware test, 2026-10-13) |
| i915 attaches headless (`nmxintel=1 nmxnofb=1`) | **Failed once**: kernel panic in Type-C port init (`icl_tc_phy_hpd_live_status`, NULL `pch_hpd`) because i915 could not see the chipset's LPC bridge. Fix applied in `bsdcompat/src/nmx_bus.c`; **retest pending** |
| DRM nodes (`NMDRM`) and Vulkan through ANV on the real device | Not tested |
| `NMXAccel` accelerator shows a Metal device on Sonoma | Not tested. Most likely stage to need code changes |
| Display output through the driver | Not tested |

The upstream project is written and tested for macOS 15. Sonoma is a port of a port: treat every row marked "not
tested" as unknown.

## Architecture

The idea: don't write a GPU driver from scratch. Run Intel's existing open-source Linux driver as unmodified as possible
inside the macOS kernel, by supplying the kernel services it expects, then put a Metal front end on top.

```
 Metal app
    │
    ▼
 Metal plugin  (plugin/, loaded by the OS from /Library/GPUBundles)
    │  detects an Intel accelerator and loads the Vulkan driver below
    ▼
 Mesa ANV  (Intel Vulkan)  +  libdrm for Darwin        gpu-port/patches/
    │  ioctls on /dev/dri/renderD*
    ▼
 ─────────────────────────── user space / kernel boundary ───────────────────────────
    │
    ├─ NMDRM.kext      DRM device nodes (/dev/dri/*) for libdrm              kexts/NMDRM
    ├─ NMXAccel.kext   IOAccelerator that WindowServer / IOSurface use       kexts/NVRM/accel (-DNMX_ACCEL)
    ├─ NMIntelFB.kext  optional: take over the panel the firmware already lit   kexts/NMIntelFB
    │
    └─ NMXGPU.kext     the GPU kernel driver (this is what the CI builds)    gpu-port/xkpi
          │
          ├─ i915           Intel GPU + display driver (Gen9 … Arrow Lake), from FreeBSD drm-kmod
          ├─ DRM core, TTM, dma-fence                                       from drm-kmod
          ├─ LinuxKPI       Linux kernel API emulation                      from FreeBSD sys/compat/linuxkpi
          ├─ bsdcompat/     FreeBSD kernel API (newbus, busdma, sysctl, locks, malloc …) implemented on xnu
          ├─ override/      the port's own replacements for upstream files (PCI, devres, DRM glue, display bridge)
          └─ xnu/           IOKit glue: the NMXGPU IOService, PCI, DMA, devfs, framebuffer
                │
                ▼
          macOS kernel (IOKit, IOPCIFamily, IOACPIFamily, IOGraphicsFamily) ──► Iris Xe GPU
```

For Tiger Lake only the **i915** driver is needed. Intel's newer `xe` driver (Lunar Lake and later) is excluded
from this build (`NMX_NO_XE=1`).

### How each layer is built

| Layer | What it is | Origin |
|---|---|---|
| `i915`, DRM core, TTM | Intel and Linux graphics code, compiled as-is (900 core + 274 i915 sources) | FreeBSD `drm-kmod` |
| LinuxKPI | Linux kernel API shims, so Linux driver code compiles unchanged | FreeBSD `freebsd-src` |
| `bsdcompat/` | The FreeBSD kernel API LinuxKPI sits on, reimplemented on xnu | NullMoth |
| `override/` | Files that replace upstream ones, plus small Python patch scripts (`fork_*.py`) that rewrite upstream code for macOS | NullMoth |
| `xnu/` | The IOKit class `NMXGPU` (PCI matching, interrupts, DMA, `/dev` nodes, display bridge) | NullMoth |

Because most of the code is fetched from upstream at build time rather than stored here, **the upstream versions matter**
(see [Pinned upstream versions](#pinned-upstream-versions)).

### How the driver is switched on

The kext matches every Intel and AMD display-class PCI function (`IOPCIClassMatch 0x03000000`), but its `probe()`
**declines every device unless you opt in**, so installing it does nothing by itself:

| Boot argument | Effect |
|---|---|
| `nmxintel=1` | Opt the Intel GPU in (or set the `nullmoth-gpu` device property). Without it macOS keeps the device. |
| `nmxnofb=1` | Attach headless: i915 and the accelerator start, but no display bridge is published and the panel is not re-lit by the driver. |
| `-nvoff` | Park every NullMoth GPU driver for one boot (recovery). |
| `-v` | Verbose boot, so `NullMoth GPU:` log lines are visible. |

There is also a boot guard: the kext sets the NVRAM variable `nullmoth-gpu-attaching` during attach. If a later boot
finds it still set (the attach never returned), the driver parks the GPU and logs why. Clear it with
`nvram -d nullmoth-gpu-attaching` or an OpenCore NVRAM reset.

## Building

### GitHub Actions (recommended)

1. Push this repository to your own GitHub repo.
2. **Actions → "NMXGPU Intel (i915) kext build" → Run workflow.** Defaults: target `14.0`, xe off, upstream commits pinned.
3. Open the run summary for per-stage results, then download the `nmxgpu-macos-14` artifact for `out/NMXGPU.kext` and
   the full logs (`ci/logs.tgz`).

The workflow runs `gpu-port/xkpi/ci-build-intel.sh`, which fetches the upstream sources, then runs `all.sh`, `all-i915.sh`,
`rt.sh` and `kext/build.sh`. It never stops at the first failure, so one run reports everything that is broken.

### On a Mac with Xcode

```bash
NMX_MIN_OS=14.0 gpu-port/xkpi/ci-build-intel.sh ~/nmx-build
# result: ~/nmx-build/out/NMXGPU.kext, summary in ~/nmx-build/ci/summary.md
```

| Variable | Default | Meaning |
|---|---|---|
| `NMX_MIN_OS` | `14.0` in CI | `-mmacosx-version-min` for objects and link |
| `NMX_NO_XE` | `1` in CI | Leave the `xe` driver out |
| `DRM_KMOD_REF` | pinned commit | drm-kmod branch, tag or commit |
| `FREEBSD_REF` | pinned commit | freebsd-src branch, tag or commit |

### Pinned upstream versions

The last known-good build used:

- `freebsd/drm-kmod` `c16de0f9fa84f70b9551f69fde837a7f3a73c0e9`
- `freebsd/freebsd-src` `b42793d4bcdf73475b77a26c9dbacd45f8876621` (`sys/compat/linuxkpi`, `sys/contrib/ck`, `sys/contrib/dev/acpica/include`)

Tracking `master`/`main` broke the first build, because FreeBSD moved code the port depends on. If you bump these, expect
to fix the port. The first build failed in three places:

| Break | Cause | Fix in this fork |
|---|---|---|
| `call to undeclared function '__const_bitcount8'` (60 sources) | FreeBSD's `hweight*()` now needs compile-time popcount macros that `bsdcompat/include/sys/cdefs.h` lacked | Added `__const_bitcount8/16/32/64` to `cdefs.h` |
| `use of undeclared identifier 'pci_lock'` in `linux_pci.c` | Upstream renamed these PCI globals to `lkpi_pci_*` and made them static; `override/src/linux_pci.c` still uses the old ones, which `linux_compat.c` defines | Added `extern` declarations in `linux_pci.c` |
| `duplicate symbol '_kmod_info'` at link | `rt.sh` compiled the NPU kext's entry point into the shared `xnuobj/` that the GPU kext links | `rt.sh` and `kext/build.sh` exclude `nmx_npu_kext` |

## Safety

Read this before loading anything.

- **The driver targets the GPU that probably drives your panel.** On a laptop, the built-in screen is wired to the
  Iris Xe. Attach reads and initialises display hardware (DMC firmware, power wells) even with `nmxnofb=1`. A bug can
  mean a black screen or a hard hang.
- **Use a recovery path.** Keep OpenCore on a USB stick, know how to reset NVRAM, and have a second machine or SSH
  access so you can read logs when the screen is dark. Make a full backup first.
- **Start headless.** Always begin with `nmxintel=1 nmxnofb=1 -v`. Only try display output after every earlier stage works.
- **The kext is unsigned.** Loading it on Sonoma needs SIP reduced and Apple Secure Boot off (`SecureBootModel Disabled`),
  as in the upstream NVIDIA setup. Understand what that costs your machine's security first.
- **Not a daily driver.** Do not install this on a machine you depend on.

## Bring-up plan

Move to the next stage only when the previous one passes. Full notes are in
[`docs/INTEL-TIGERLAKE-SONOMA.md`](docs/INTEL-TIGERLAKE-SONOMA.md).

1. **Validate the kext statically** on the Mac: check that its symbols resolve against Sonoma's kernel before it ever loads.
2. **Attach, headless.** Boot args `nmxintel=1 nmxnofb=1 -v`. Pass: the log shows `NullMoth GPU: driving 8086:9a49` and
   the desktop still works. Fail: capture the panic or hang, because the first failing function is the next work item.
3. **Device nodes.** Build and load `NMDRM` (`NMX_MIN_OS=14.0`); `tools/drmlist` should list `8086:9a49` with a render node.
4. **Vulkan.** Run `vkprobe` through ANV on the real render node: create a device, then compile a compute pipeline.
5. **Accelerator.** Build `NMXAccel` (`build/nmxaccel_build.sh`) and see whether `system_profiler SPDisplaysDataType`
   reports a Metal device. This is the stage most likely to need source changes on Sonoma: it subclasses Apple
   IOAccelerator classes from reconstructed headers laid out for macOS 15 and 26.
6. **Display.** Try without `nmxnofb`, and compare against `NMIntelFB`.

Needed besides the kext: Intel firmware for Tiger Lake (`i915/tgl_dmc_*`, `tgl_guc_*`, `tgl_huc_*`, listed with
checksums in `app/Resources/nullmoth-gpu-firmware.txt`), and for later stages the userland in `gpu-port/patches`.

### What to report after a test

The `NullMoth GPU:` and `NullMoth xkpi:` log lines:

```bash
log show --last boot --predicate 'eventMessage contains "NullMoth"'
kmutil showloaded --list-only | grep -i nullmoth
```

plus the panic text if there was one, and which stage you reached.

## Known limits (from the upstream port)

- HDCP, writeback connectors and the panel-brightness quirk table are not offered (their DRM helpers are GPL).
- dma-buf and sync_file file descriptors are not supported (a kext cannot create them); PRIME export reports `EOPNOTSUPP`.
- Display: one mode (the largest output's), other outputs mirror the top-left; no mode switching yet.
- DMI quirks see the Mac model the bootloader presents, not the PC's real board.
- `package/install.sh` still installs from `/Library/NullMoth/kexts/15/`; the install path assumes macOS 15 and has
  not been adapted for Sonoma.

## Repository map

| Path | Contents |
|---|---|
| `gpu-port/xkpi/` | The AMD/Intel kernel port: build scripts, `bsdcompat/`, `override/`, `xnu/`, `kext/` |
| `gpu-port/patches/` | Mesa (RADV/ANV) and libdrm patches for macOS |
| `kexts/NMDRM`, `NMIntelFB`, `NVRM/accel` | DRM nodes, panel takeover, Metal accelerator |
| `plugin/` | Metal driver plugin that selects the Vulkan back end |
| `.github/workflows/nmxgpu-intel-build.yml` | The CI build |
| `docs/INTEL-TIGERLAKE-SONOMA.md` | Detailed Tiger Lake / Sonoma bring-up notes |
| `docs/README-NVIDIA.md` | Original NVIDIA driver README |
| `nvk/`, `translator/`, `kexts/NVRM*`, `app/` | The NVIDIA side of the project; not used for Intel |
| `gpu-port/kext-npu/`, `gpu-port/npu-user/` | NPU driver (GPL-2.0); not part of the Iris Xe build |

## License

This fork keeps the upstream licensing; it does not change it.

- NullMoth's code (`plugin/`, `kexts/`, `build/`, `package/`) is **PolyForm Noncommercial 1.0.0** (`LICENSE`): personal,
  research, educational and non-profit use only. **Commercial use, including selling builds, is not allowed.**
- `translator/` is LGPL-3.0-or-later. Mesa changes are MIT.
- The kernel port compiles code from FreeBSD `drm-kmod` and `freebsd-src` at build time, and the NPU driver is
  GPL-2.0. Check the licenses of any source you redistribute. See `NOTICE`.
- GPU firmware belongs to Intel and is redistributed only under its own license.

## Credits

[NullMoth Systems](https://github.com/nullmoth/nvidia-macos-driver) for the driver and the AMD/Intel port;
the FreeBSD `drm-kmod` and LinuxKPI maintainers; Intel's i915 and Mesa ANV developers.
