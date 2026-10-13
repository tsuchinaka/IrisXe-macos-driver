# Intel Iris Xe (Tiger Lake, 8086:9A49) on macOS 14 Sonoma: bring-up notes

Status: **experimental, untested on hardware.** These notes cover how to try the repo's existing Intel path
(NMXGPU i915 + NMXAccel + Mesa ANV) on a Tiger Lake laptop that already boots Sonoma through OpenCore without
graphics acceleration. The repo is written and tested for macOS 15; nothing here has been run on 14 yet.

## What is in the repo for this chip

| Piece | Where | Notes |
|---|---|---|
| Kernel driver (i915, Gen12) | `gpu-port/xkpi` -> `NMXGPU.kext` | Tiger Lake is covered by i915; xe is not needed for it |
| Accelerator for WindowServer / IOSurface | `kexts/NVRM/accel` built with `-DNMX_ACCEL` -> `NMXAccel.kext` | built against macOS 15 (and 26) class layouts; not checked against 14 |
| DRM device nodes for userland | `kexts/NMDRM` | `/dev/dri/renderD*` for libdrm |
| Vulkan driver | Mesa ANV, `gpu-port/patches/mesa-macos.patch` | Gen12 is in the 21-chip Intel test matrix |
| Metal plugin | `plugin/` (loads ANV when the accelerator is Intel) | |
| GPU firmware | `app/Resources/nullmoth-gpu-firmware.txt` | `i915/tgl_dmc_*`, `tgl_guc_*`, `tgl_huc_*` are in the manifest |
| Panel takeover (no acceleration) | `kexts/NMIntelFB` | `0x9a49` is in `display_ids.inc` as display version 12 |

## Building with GitHub Actions (recommended)

`.github/workflows/nmxgpu-intel-build.yml` builds `NMXGPU.kext` for the Intel (i915) path on macOS runners (14 and 15,
both cross-compiling to x86_64) with one script, `gpu-port/xkpi/ci-build-intel.sh`. The script fetches the sources
itself (drm-kmod `master`, plus `sys/compat/linuxkpi` and `sys/contrib/ck` from freebsd-src), generates `amd.flags`,
then runs the same stages as the author's build: `all.sh`, `all-i915.sh`, `rt.sh`, `kext/build.sh`. It does not stop at
the first failure, so one run shows every broken stage.

1. Put this repository in your own GitHub repository (a fork or a new private one).
2. Actions > "NMXGPU Intel (i915) kext build" > Run workflow. Defaults: target 14.0, xe left out.
3. Open the run's summary: it lists the resolved commits of drm-kmod and freebsd-src, the per-stage result, the top
   compile errors, and whether the kext linked. Download the `nmxgpu-macos-14` / `nmxgpu-macos-15` artifact for the full
   logs (`ci/logs.tgz`) and, if it linked, `out/NMXGPU.kext`.

The same script runs on any Mac with Xcode: `NMX_MIN_OS=14.0 gpu-port/xkpi/ci-build-intel.sh ~/nmx-build`.

The first run is expected to fail somewhere. Nobody has built this tree outside the author's machine; the point of the
run is to find where.

## Building by hand

The xkpi build scripts used to hard-code the author's build machine. They now read these variables, with the old
values as defaults:

| Variable | Default | Meaning |
|---|---|---|
| `NMX_X` | `/Volumes/MiniData/build/gpu-src/xkpi` | the `gpu-port/xkpi` working tree (holds `amd.flags`, `obj*/`) |
| `NMX_K` | `/Volumes/MiniData/build/gpu-src/kpi` | directory holding the `drm-kmod` and `freebsd-src` checkouts |
| `NMX_PY` | `/opt/homebrew/bin/python3` | Python used by `srclist.py` and the `fork_*.py` scripts |
| `NMX_LINUX614` | `/Volumes/MiniData/build/gpu-src/linux-6.14` | Linux 6.14 tree, only for the xe step (`all-xe.sh`) |
| `NMX_MIN_OS` | `15.0` | `-mmacosx-version-min` for the xkpi kext objects, the NMXGPU link and `kexts/NMDRM` |

For a Sonoma target set `NMX_MIN_OS=14.0` for every step, so the Mach-O build version is not newer than the OS:

```bash
export NMX_MIN_OS=14.0 NMX_X=$PWD/gpu-port/xkpi NMX_K=$HOME/gpu-src/kpi
cd gpu-port/xkpi
./all.sh && ./all-i915.sh && ./rt.sh      # then the xe step if kext/build.sh still links it
./kext/build.sh /tmp/out
```

Open items on the build side:
- `kext/build.sh` links the xe objects unless `NMX_NO_XE=1` is set (the CI script sets it). Whether the kext links without
  xe has not been tried.
- `kexts/NVRM/accel` needs NVIDIA's `open-gpu-kernel-modules` headers (`OGKM`) only because the accelerator source is
  shared with the NVIDIA build; `nmxaccel_build.sh` has no deployment-target flag yet.
- The `-ffile-prefix-map` path in the `cc*.sh` scripts is still the original one; it only affects embedded paths.
- The sources the scripts expect (drm-kmod, FreeBSD `sys/compat/linuxkpi`, `amd.flags` from `gen_flags.py`) are not in
  this repository.

## Boot arguments

| Boot argument | Effect |
|---|---|
| `nmxintel=1` | opt the Intel GPU in to NMXGPU (without it the driver declines and macOS keeps the device) |
| `nmxnofb=1` | **new.** Attach headless: i915 attaches and NMXAccel starts, but the display bridge is not published. No DRM client lights the outputs and no IOFramebuffer is added. |
| `-nvoff` | parks every NullMoth GPU driver for that start (recovery) |
| `-v` | verbose boot, so the `NullMoth GPU:` lines can be read |

`nmxnofb=1` is meant for the first test on a laptop whose panel is wired to this iGPU. It does not stop i915 from
reading out and initialising the display hardware during attach (DMC firmware load, power wells); it only removes the
bridge that re-lights the panel with its own buffer. If the screen still goes dark during attach, that is the next
thing to fix, and the boot log is what shows it.

The kext also keeps its own boot guard: an NVRAM marker (`nullmoth-gpu-attaching`) is set during attach, and a start
that finds it still set parks the GPU with macOS and logs why. Clear it with `nvram -d nullmoth-gpu-attaching`, or
from OpenCore's NVRAM reset.

## Stages

Move to the next stage only when the previous one passes.

1. **Attach, headless.** Boot args `nmxintel=1 nmxnofb=1 -v`. Pass: log has `NullMoth GPU: driving 8086:9a49` and the
   desktop still shows on the panel.
   Fail: kernel panic or hang during attach. Keep the photo or serial log; the first failing function is the
   next work item.
2. **Device nodes.** With `NMDRM` loaded, `tools/drmlist` should list `8086:9a49` with a render node.
3. **Vulkan.** `vkprobe` through ANV on the render node: device creation, then a compute pipeline (as in
   `tools/vk_chip_matrix.sh`, but on the real device instead of the drm-shim).
4. **Accelerator on Sonoma.** `NMXAccel` loads and `system_profiler SPDisplaysDataType` shows a Metal device. This is the
   stage most likely to need source changes, because the accelerator subclasses Apple's IOAccelerator classes from
   reconstructed headers (`kexts/NVRM/accel/re`, `iofam`) and has a forwarding layer only for macOS 26.
5. **Display.** Only after 1-4 work: try without `nmxnofb`, and compare with `NMIntelFB`.

## What to send back after a test

The `NullMoth GPU:` and `NullMoth xkpi:` lines from `log show --last boot --predicate 'eventMessage contains "NullMoth"'`
(or the verbose boot photo), the output of `kmutil showloaded --list-only | grep -i nullmoth`, and for a panic the
panic text. Name the stage reached.
