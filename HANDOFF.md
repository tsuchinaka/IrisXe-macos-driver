# Iris Xe macOS acceleration — status handoff (2026-10-21, build 116 = b115 + MODERN display classes behind boot-arg nmxmodern=1 (DESIGN.md R4))

Machine: Xiaomi Mi Notebook Pro, Tiger Lake Iris Xe (8086:9a49), macOS Sequoia 15.8.1, OpenCore.
Builds: GitHub Actions, workflow `.github/workflows/nmxgpu-intel-build.yml` (manual run). Output: `out/NMXGPU.kext`
and `out/NVMTLDriver.bundle` (Metal plugin). Zips never include `github-workflows-copy/` or `push-to-github.sh`.

## Design in one paragraph
NMXGPU.kext = FreeBSD drm-kmod i915 ported to XNU (`gpu-port/xkpi`), with the framebuffer `NMXGPUFB`
(`gpu-port/xkpi/xnu/nmx_fb.cpp`, bridge `gpu-port/xkpi/override/src/nmx_fb_bridge.c`). NMXAccel / NVRMAGDC
(`kexts/NVRM/accel`) supply the IOAccelerator classes WindowServer expects. NVMTLDriver.bundle (`plugin/`) is a
Metal driver that translates to Vulkan (NVK-style, `plugin/nvmtl_vk.c`). WindowServer presents via legacy surface
swap (`NVSurface::submitSwapCopy`) and a ~1 ms CPU copy to scanout. Only processes listed in
`/Library/GPUBundles/nvmtl-allow.txt` get the Metal plugin (only WindowServer today; `!` prefix = armed only).

## Boot-args (working config)
`keepsyms=1 debug=0x100 -vi2c-force-polling nmxintel=1 nmxaccel=1 -nmintelfboff nmxws=1 amfi_get_out_of_my_way=0x1 amfi=0x80 ipc_control_port_options=0`
Optional: `nmxgpumhz=800` (GPU min clock, default 800, 0=off), `-nmxnohwcursor`, `nmxpresent=1` (b93 path, off),
`nmxautoarm=1|2|3` (Phase 3 helper v4: 1 full, 2 dry run, 3 arm only).

## Arming acceleration (manual, after login) — `tools/macos-helpers/nmx-ws-arm.sh`
`sysctl debug.nvaccelfb=1` → `=4` → `debug.nvrmfb_agdc_k5=1` → `debug.nvrmfb_agdc=1` → `debug.nvaccel_iop=1` → `killall WindowServer`.
System Settings > Accessibility > Display > **Reduce transparency must be ON** before arming (else desktop freezes).
If the driver does not load after a forced power-off: `sudo nvram -d nullmoth-gpu-attaching`.

## WORKING
- i915 driver loads, display/modeset, native resolution, hotplug.
- Armed desktop is stable: WindowServer renders through the Metal plugin on the Iris Xe.
- Login with acceleration (b96: read-only userptr pages pinned read-only — fixed the login loop).
- Hardware cursor (b94/b95): smooth, no ghosts, survives login/lock refresh.
- VBL with two clients (b102 fix), real 60 Hz pacing, swap copy ~1 ms.
- GPU clock floor 800 MHz (b101; RPS had no up-interrupts, clock was stuck at 100 MHz).
- Window dragging smooth, Sonoma/Ventura wallpapers.
- Metal plugin is built in CI (b97+); framebuffer fetch (b99) is ON by default since b114 (off: `/Library/GPUBundles/nvmtl-nofbfetch`).

## NOT WORKING / OPEN
1. **Residual lag** (b106 finding: WindowServer composites the screen with CompositorSW, see Phase 4 notes): small typing delay, System Settings icons load late, occasional garbage bands in some app
   windows (e.g. Claude app). Cause: apps draw on the CPU and WindowServer composites app layers + blur with
   CoreAnimation SWContext (CPU). Tried and NOT the cause: GPU clock, zero-copy off (`NVMTL_SURFACE_HOST=0`),
   ws-nowait, programmable blending, display-pipe timeouts (none), CPU speed (3.7 GHz).
2. **Reduce transparency OFF + armed = freeze** (blur path on GPU broken).
3. **Sequoia / Macintosh (and video) wallpapers show white** — need GPU paths not implemented.
4. **Phase 3 auto-arm: WORKS 10-21 on b112** (dry run m2, arm-only m3, full m1 all passed; m1 restarted the login-window WindowServer 165 -> 782, stable 60 s, no panic). New report from the user after m1: occasional BLACK SQUARE BOXES (not yet diagnosed) + the known typing delay. Old note: freezes the Mac — CAUSE FOUND 10-21 (see Phase 4 notes, b112 guard) (v1, v2: black screen after Apple logo, no SSH, no Caps Lock, no log).
   Facts (b103 review): the kernel never reads `nmxautoarm`, so the boot-arg alone cannot freeze anything. WindowServer
   starts BEFORE NMXAccel exists (NMXGPU attaches only after the disks mount), so an in-kernel arm cannot avoid one
   WindowServer restart - the NVIDIA-style boot hold (NVRM::autoGo) does not fit this machine. v3 bug: its "90 s after
   boot" wait parsed `usec` from kern.boottime, so it never waited (fixed in v4).
   v4 (`tools/macos-helpers/nmx-autoarm/`): modes 2 dry run / 3 arm only / 1 full, NVRAM breadcrumb per step
   (`nmx-autoarm-step`, "m<mode> <step>"), guard trips ONLY if the last boot stopped in an ARM or RESTART step, waits for
   WindowServer to open NMXGPUFB + power registered + a VBL client (b103 ioreg props NMXFBOpened / NMXFBPower /
   NMXFBVBLClients), WindowServer same pid 45 s, >= 120 s since boot. Report: `nmx-autoarm-report.sh`
   -> /Users/Shared/nmx-autoarm-report.txt (breadcrumbs, panic reports, log). NOT TESTED YET.
   Test ladder: A) nmxautoarm=1 helper NOT installed, B) mode 2, C) mode 3, D) mode 1. The first that freezes is the cause.
5. Screenshots black unless Terminal has Screen Recording permission (expected).

## Roadmap
Phase 3 auto-arm (DONE b112, nmxautoarm=1) → 4 apps on GPU (allow-list more processes, fix their Metal paths) →
5 blur on GPU → 6 real page flip (replace CPU copy) → 7 sleep/wake → 8 video decode → 9 cleanup.

## Phase 4 (apps on the GPU) - started in b104, user chose Phase 4 before testing Phase 3
Plan: test apps ONE at a time, simple -> heavy, and fix the Metal paths each one breaks, from its report.
Order: mtlwindow (baseline, CAMetalLayer -> WindowServer) -> TextEdit -> Calculator -> System Settings (+ its pane
extension processes) -> Finder -> Safari + com.apple.WebKit.GPU -> Claude ("Claude Helper (GPU)", Electron/ANGLE).
Tool: `tools/macos-helpers/nmx-app-gpu.sh` (also in CI artifact out/metal-tests/ with mtlprobe/mtldraw/mtlwindow):
`test "<App>" [helper procs]` adds `!<name>` (armed-only), opens the app, watches 30 s, verdict + report
/Users/Shared/nmx-app-<App>.txt (plugin log lines from the unified log - sandboxed apps cannot write /tmp/nvmtl.log -,
crash reports, WindowServer pid). Auto-removes on crash. Freeze guard NVRAM `nmx-app-test`: cleared after the 30 s; if
still set, nmx-app-gpu.sh / nmx-ws-arm.sh / autoarm take those names off the list first (log /Users/Shared/nmx-app-frozen.txt).
Already in the plugin: sandbox DRM bridge (b80), armed-only entries, MetalPluginName restored on arm.
Results table (fill in): app | got device | runs | looks right | faster | report
- TextEdit (b104 plugin, 10-20): allow-listed, but zero Vulkan devices: the app sandbox refuses opendir(/dev/dri)
  (EPERM) - stat and the IOKit bridge are allowed, open() is refused. libdrm never listed a node, so the bridge's open
  hook never ran. FIX b105: bridge hooks opendir (`_opendir$INODE64`/`_opendir` in libvulkan_intel + libdrm) and
  returns a rebuilt listing (empty files named after the nodes that stat as char devices, in $XDG_RUNTIME_DIR/nvmtl-dri).
  The b104 test script also reported a false crash (an old bluetoothd report); b105 counts only reports of the tested
  processes / WindowServer and says "GPU did not start inside it" for the refusing case.
- TextEdit (b105 plugin, 10-20): Vulkan device UP in the sandbox ("Intel(R) Iris(R) Xe Graphics (TGL GT2)"). Then
  QuartzCore default.metallib (161 blobs): "in-memory bitcode reader unavailable" (air-runtime/libLLVM.dylib MISSING -
  deleted when the whole bundle was replaced with the CI one, which has only the binary), air-opt fallback aborts (wait
  status 6: its libLLVM is in the same air-runtime), "translation produced nothing" -> TextEdit quit. Also: the app used
  its EMPTY private cache (shared /Library/GPUBundles/nvmtl/aircache is not writable from the sandbox, and the cache was
  only used when writable). FIX b106: private miss -> read the shared aircache/spvcache (read-only, nvmtl_shared_twin);
  tools/macos-helpers/nmx-plugin-update.sh replaces only the binary and can restore air-runtime from the
  mesa-anv-build.yml artifact (out/nmx-metal/NVMTLDriver.bundle). NEVER rm -rf the installed bundle.
- TextEdit (b106 plugin + air-runtime restored, 10-20 16:17): RUNS ON THE GPU 30 s, no crash. CoreAnimation pipelines
  built (fixed_frag_lph_cph etc.), zero-copy IOSurface out. Cost: first launch compiles each function-constant variant
  ~2.2-2.6 s (fc cache miss, 595 KB SPIR-V each) - cached per app afterwards. Shared cache gave 0 hits (WindowServer's
  cache is not in /Library/GPUBundles/nvmtl/aircache either, or keys differ) - to look at: pre-filling a shared cache.
  Look/feel not reported yet. Test script now joins curly-quoted names, undoes the list on a failed open, `clean` command.
- b106, 10-20 16:3x: Calculator, System Settings, Safari, TextEdit all RUN ON THE GPU 30 s, no crash, WindowServer
  stable. User: "no lags and no crashes", but System Settings icons still load late on fast scroll and typing still has
  a delay. System Settings' own process made a device but its log shows NO drawing (no QuartzCore library) - its panes
  are separate extension processes (.appex), likely still on the CPU. Next: `tools/macos-helpers/nmx-lag-probe.sh`
  (Apple `sample` of WindowServer + app + appex/WebKit helpers for 10 s while typing/scrolling, census CPU compositor /
  pixel copy / text / icon decode / GPU / waits) -> decide: WindowServer CPU compositing (HANDOFF item 1) vs app-side.
- Lag probe v1 (TextEdit typing, 10-20 17:02, raw samples): NOT CPU-bound. WindowServer main thread busy ~5%
  (display update path ~156 ms in 8 s; CompositorMetal does the composite, CompositorSW only shadows/small parts;
  100 of 102 render_update samples sit in IOAccelWriteLockSurfaceWithOptions = waiting in the kernel for the display
  surface). TextEdit main thread busy ~4%; its waits are NSSpellChecker / text-candidate XPC (spell server, inline
  predictions) ~2 s per 8 s on background queues. Leads: (1) text services in another process, (2) per-frame display
  surface lock wait (copy path; Phase 6 page flip). Probe v2: top (busiest processes) + per-app raw folders.
- Lag probe v2, System Settings fast scroll (10-20 17:09) + raw ws.txt: System Settings main thread 46% busy (SwiftUI /
  AttributeGraph, icon decode - app CPU work). WindowServer: THE SCREEN IS COMPOSITED ON THE CPU. 317 ms main thread
  in run_timer_pass > update_display_callback > CGXUpdateDisplay > UpdateDisplays > CDDisplay::render_update >
  CompositorSW::CompositeLayersToDestination, + 410 ms in layer_blit_entry_block workers; CA renderer is SWContext
  (212 lines) vs MetalContext (4); CompositorMetal only for CaptureSurfaceMetal (flatten). CORRECTION: the probe v1/v2
  "GPU compositor" count matched CA::OGL, which is mostly CA::OGL::SW - wrong label. Typing delay unchanged with spell
  check + predictive text off (user) -> not text services. Real target: why CoreDisplay picks CompositorSW for the
  NMXGPUFB display (legacy display surface: CGXAccessDisplayDeviceSurface / IOAccelWriteLockSurface / submitSwapCopy)
  instead of the Metal compositor with display-pipe (IOPresentment) transactions. nmx-ws-check.sh now measures
  iop_ok over 5 s and greps WindowServer's compositor messages.
- nmx-ws-check (10-20 17:23): display-pipe transactions UNUSED (iop_ok +0, "pipe vbl 0/0 tx 0/0 submit 0") - the
  legacy swap path (submitSwapCopy) is the only one. WindowServer logs "CoreDisplay copy_forward_region: copy forward
  failed (0xe00002c7) ... disabling" ~30x a second (unsupported answered by our side) -> CoreDisplay cannot reuse
  unchanged pixels. Swap stats: EXACTLY half the swaps copied every second ("frames 66, copied 33" ...): every other
  frame never reaches the panel (alternating back buffer without content/kva? or nvAccelResHasContent's 16 sample
  pixels all 0). Next: tools/macos-helpers/nmx-copyfwd-trace.sh (dtrace, SIP off: failing IOConnectCallMethod
  selectors + ustack, kernel functions returning 0xe00002c7, HasContent/copy results).
- dtrace (10-20 17:31, nmx-copyfwd-trace.sh): WindowServer selector 9 -> IOAccelSurface::s_set_shape -> 
  IOAccelLegacySurface::copy_forward -> submitCopyForward returned 0xe00002c7 (242/242 in 8 s; the family default - the
  driver never overrode it). 484 swaps: nvAccelResHasContent 1 x242, 0 x484 -> every other swap both a2 and a1 "empty"
  (the 2nd back buffer only holds freshly drawn parts: no copy forward). FIX b107: NVSurface::submitCopyForward
  (NMX_ACCEL, not Tahoe) -> nmxCopyForward: CPU memcpy of the IOAccelBounds {int16 x,y,w,h} rects from the buffer last
  copied to the panel (nmxNoteFront in submitSwapCopy) into the other; no rects = whole buffer. Header: submitCopyForward
  returns IOReturn (was void). Frame line now ends "| copyfwd ok N fail M"; first 12 calls logged "copyfwd: ...".
  Off switch: boot-arg nmxnocopyfwd=1. Expected: "copied" = frames, WindowServer error gone, no garbage bands, less lag.
- b107 RESULT (10-20 18:04, photo): armed login screen showed stale pixels around the cursor / password field and
  login did not work -> copy forward copied from a stale buffer. Cause: in the family's model swap a1 = FRONT buffer
  and copy forward copies from it, but our swap only copied a2 -> panel, never into a1; b107 also guessed a source when
  neither argument was known. FIX b108: swap keeps a1 = what the panel shows (nmxSwapKeepFront, a2 -> a1); copy
  forward source = a1 or the buffer last shown, else unsupported (no guessing); logs a/b/a1/shown pointers. OPT-IN:
  boot-arg nmxcopyfwd=1 (default off = b106 behaviour). If b108 still corrupts: check the logged rects (IOAccelBounds
  layout assumed int16 x,y,w,h) and which argument is a1.
- b108 + nmxcopyfwd=1 (10-20 18:20): login works, every frame copied, copyfwd ok 215 fail 1 - but heavy flicker
  between the desktop and the OLD LOCK SCREEN. Swap trace (tools/macos-helpers/nmx-swap-trace.sh, one-shot
  LaunchDaemon + dtrace, 18:49): ONE surface; every update = copyfwd a1 -> B1, swap(a1,B1), 2 ms later swap(a1,B2).
  B2 never gets a copy forward nor drawing: it held the lock screen; the panel showed it after every real frame. (b106:
  B2 was all zero -> HasContent skipped it -> the "every other frame" pattern.) FIX b109: nmxSwapIsStale - a back
  buffer is shown only if it was the last copy-forward target or its 48x48-sample signature changed since it was last
  shown (a never-seen buffer counts as new only before the first copy forward). Frame line: "stale-skipped N".
- b109 (10-20 19:07 photo): no lock-screen flicker reported, but EVERY Dock label the pointer passed stayed on
  screen (also on the lock screen) + lag. Cause: label removal is drawn into B1 without a copy forward; the 48x48
  signature missed it -> frame skipped -> a1 kept the label -> copy forward copied it back. FIX b110: a back buffer is
  skipped only if it has NEVER been a copy-forward target (and a copy forward happened in the last 1 s). LAG: copy
  forward into the buffer already on the panel is a no-op, and the a2 -> a1 front copy is lazy (only when copy forward
  needs a1 for another buffer): 1 full-screen copy per frame instead of 3.
- b110 (10-21 03:33): lock screen shown, desktop only flashing through. Causes: (1) b110 showed every buffer after
  1 s without a copy forward -> the unused buffer B2 (lock screen) reached the panel; (2) the lazy a1 sync could copy
  from B2 (at the first copy forward or after (1)) -> lock screen planted in B1 outside the damage. FIX b111: no timer;
  never-target buffers are never shown nor used as a1 source; first copy forward (and first after a1 changes = buffers
  rebuilt, nmxSwapNoteA1) copies nothing and drops the pending a1 sync. Bench replay of the 18:49 trace pattern: panel
  shows B1 every update.
- b111 + nmxcopyfwd=1 (10-21 03:56): WORKS - no flicker, no stale pieces. Typing delay unchanged (user). Next:
  tools/macos-helpers/nmx-type-latency.sh (dtrace: TextEdit insertText -> CA commit -> next submitSwapCopy, frame gaps).
- Typing latency (10-21): delay ONLY when armed (user). nmx-type-latency: TextEdit insertText -> CA commit 0 ms,
  -> next submitSwapCopy 13 ms avg (25 worst; may be an unrelated swap). nmx-key-latency: kernel key event ->
  TextEdit keyDown 3 ms armed and unarmed (worst 13 vs 4); WindowServer surface_lock_options waits 106 x ~2 ms in 15 s.
  PSR/FBC are already 0 at build (all-i915.sh). Next: nmx-type-pipeline.sh (A insertText -> B TextEdit commit ->
  C WindowServer commit_transaction -> D UpdateDisplays -> E submitSwapCopy), armed vs unarmed.
- nmx-type-pipeline v2 (10-21 04:18/04:19), 43 / 32 letters: key in TextEdit -> WindowServer display update 15 ms
  armed vs 14 ms unarmed; -> WindowServer renders the app layers (WSCALayerBacking::RunUpdate) 15 vs 14 ms (worst 46
  vs 25); armed -> frame at the panel copy 16 ms avg (worst 47). The pipeline is the SAME armed and unarmed up to the
  panel; armed only adds occasional extra-frame spikes. Both modes composite on the CPU (CA SWContext / CompositorSW):
  armed mode is not "full acceleration" for typing. The real speed-up needs WindowServer on CompositorMetal through
  the display pipe (IOPresentment transactions, iop_ok > 0) + page flip (Phase 6) - currently iop_ok stays 0.
- PHASE 3 FREEZE CAUSE (10-21, Kernel-2026-10-20-132943.panic, nmxautoarm=1, uptime 10.9 s, process sysctl):
  nvaccelfb_sysctl -> registerFramebufferLocked -> onCountGrewLocked -> deliverModeChange ->
  IOAccelLegacyDisplayMachine::display_mode_did_change -> IOAccelLegacyDisplayPipe::displayModeDidChange ->
  IOFramebuffer::getAttributeExt page fault at 0x120: the v1 script armed before WindowServer had opened NMXGPUFB
  (no controller yet). debug=0x100 halts on panic = black screen, no SSH. FIX b112: triggerFromSysctl refuses (logs
  "REFUSED") while NMXGPUFB lacks NMXFBOpened (needs NMXGPU b103+ - the user still ran an older NMXGPU on 10-21);
  autoarm v4 retries nvaccelfb for 60 s and never arms AGDC/iop without it; nmx-ws-arm.sh stops if refused.
  Also 10-21: the autoarm LaunchDaemon was NOT installed (dry run never ran) - reinstall with install-autoarm.sh.
User's allow-list at that time also had: !Finder !GeneralSettings !iconservicesagent !Dock !TextInputMenuAgent and an
UNARMED "Brave Browser Helper" line (gets the device even unarmed).

## Build 112
nvrm-accel.cpp: arm refused until NMXGPUFB is opened (Phase 3 panic). Install BOTH NMXGPU.kext (b103 props) and NMXAccel.kext. Scripts: autoarm v4 retry, nmx-ws-arm stop on refusal.

## Build 111
nvrm-accel.cpp / nvaccel_stubs.h: no 1 s fallback, first copy forward is a no-op, reset on new a1. Still opt-in: nmxcopyfwd=1.

## Build 110
nvrm-accel.cpp: never-a-target skip rule, no-op copy forward into the front, lazy a1 sync. Still opt-in: nmxcopyfwd=1.

## Build 109
nvrm-accel.cpp / nvaccel_stubs.h: stale 2nd-swap skip (copy forward on only). Still opt-in: nmxcopyfwd=1.

## Build 108
Same files as 107: copy forward opt-in (nmxcopyfwd=1), front buffer kept current, no guessed source.

## Build 107
`kexts/NVRM/accel/nvaccel_stubs.h` + `nvrm-accel.cpp` (copy forward), `iofam/IOAccelLegacySurface.h` + `tahoe_fwd.h` (IOReturn). Kext to install: NMXAccel.kext.

## Build 106
`plugin/NVMTLLibraryLoad.m`: shared shader cache read by sandboxed apps. `tools/macos-helpers/nmx-plugin-update.sh` (new). nmx-app-gpu.sh: missing shader reader note.

## Build 105
`plugin/nvmtl_drmbridge.h`: opendir hook (sandboxed apps). `tools/macos-helpers/nmx-app-gpu.sh`: crash filter, no-device verdict.

## Build 104
`.github/workflows/nmxgpu-intel-build.yml`: new step builds mtlprobe/mtldraw/mtlwindow -> out/metal-tests/.
`tools/macos-helpers/nmx-app-gpu.sh` (new), `nmx-ws-arm.sh` + autoarm v4: app freeze guard.

## Build 103
`gpu-port/xkpi/xnu/nmx_fb.cpp`: readiness properties only (no behaviour change); `tools/macos-helpers/nmx-autoarm/*` v4.

## Key files changed in builds 94–102
`gpu-port/xkpi/override/src/nmx_fb_bridge.c` (cursor), `gpu-port/xkpi/xnu/nmx_fb.cpp` (cursor, VBL clients, clock log),
`gpu-port/xkpi/xnu/nmx_mm.cpp` (read-only pin), `gpu-port/xkpi/override/i915/nmx_i915_awake.c` + `xnu/nmx_kext.cpp`
(GPU floor), `kexts/NVRM/accel/nvrm-accel.cpp` (frame stats, display-pipe counters), `plugin/NVMTLObjects.m`,
`plugin/nvmtl_vk.c/.h` (fbfetch), `.github/workflows/nmxgpu-intel-build.yml` (plugin build step).
Diagnostics on the Mac: `tools/macos-helpers/nmx-why.sh`, `nmx-ws-check.sh`, `nmx-boot-report.sh`.

### b113 (10-21) lock screen black boxes (armed: clock missing, date cut off at the baseline, unlock spinner black)
Untested guess, the user did not want another diagnostic run (tools/macos-helpers/nmx-blackbox.sh is ready if needed).
Cause assumed: the b109-b111 rule (a back buffer that never got a copy forward is never shown) came from a desktop trace;
on the lock screen WindowServer draws into the other buffer (b108: B2 held the lock screen), and after a buffer rebuild
the first copy forward copied nothing (new buffer = zeros = black). b113 (only with nmxcopyfwd=1): every swap shows a2,
then copies that frame into every other known back buffer of the same size (nmxSwapSync); copy forward copies its rects
from the last shown frame (nmxCopyForwardSync). Buffers tracked by kva, forgotten in deallocPhysical (nmxSyncForget).
Cost ~1 ms more RAM copy per swap. boot-arg nmxsyncoff=1 = b111 behaviour. Stat line: "b113 back buffers synced N".
- b113 result: no change (clock still black). Not the panel copy.

### b114 (10-21) plugin only: framebuffer fetch default ON
- nvmtl-ws.log showed WindowServer's CA Metal shaders (fixed_frag_lph_cph) read [[color(0)]] (binding 192) while b99 fbfetch
  was OFF -> lock screen clock/date/vibrant text black. With /Library/GPUBundles/nvmtl-fbfetch the boot-armed lock screen
  drew far fewer black boxes (user 10-21 06:34). Default is now ON; off with the file nvmtl-nofbfetch or NVMTL_FBFETCH=0.
- Hangs that day: `killall -9 WindowServer` while LOGGED IN, fbfetch on: the new WindowServer logged ~100 lines of start-up
  (psofree) and stopped, before any fetch draw. Same kind of hang without Metal (no device -> SIGABRT loop -> watchdog
  panic Kernel-2026-10-21-061004). Do not restart WindowServer by hand while logged in; reboot instead. Not understood yet.
- WindowServer cannot run armed without a Metal device (crash loop) - never take it off the allow-list.
- Unlock spinner still black: candidate = newTiledTextureWithDescriptor fallback (54x54, 6 mips) copied level 0 only;
  b114 generates levels 1+ from level 0. Untested.
- Also seen: "WindowServer: each frame is waited for before the swap (nvmtl-ws-nowait to test without)" - typing-delay lead.

### b115 (10-21) kernel: b113 sync off by default
- b114 result (user photo 06:31/06:53): clock + full date now drawn (framebuffer fetch). Left: a dark capsule behind the clock
  and the unlock spinner (blur/glow; plugin log shows downsample_blur/narrow_blur/sum_rgba running, no error - needs a
  probe of what the blur samples). b114 mip generation for the 54x54 tiled texture did not change the spinner.
- b113 sync cost up to 4 full-frame copies per update for no gain -> off (boot-arg nmxsync=1 restores it).
- User's main list 10-21: 1 typing delay, 2 small lags, 3 lock screen black glow/spinner, 4 UI not smooth, 5 System
  Settings icons late while scrolling. Next test: /Library/GPUBundles/nvmtl-ws-nowait (WindowServer stops waiting for each
  Metal frame to finish on its main thread = the input thread).

### b116 (10-21) NMXAccel: modern display classes (opt-in, boot-arg nmxmodern=1) - DESIGN.md R1/R4
- R1 (Apple Intel ICL driver from the user's kernel collection): Apple's Intel accelerator uses IOAccelSurface /
  IOAccelDisplayPipe / IOAccelDisplayMachine; NMXAccel used the Legacy* subclasses (NVIDIA era). See DESIGN.md.
- New classes (nvaccel_stubs.h / nvrm-accel.cpp, NMX_ACCEL && !NM_TAHOE): NMXSurfaceM : IOAccelSurface,
  NMXDisplayPipeM : IOAccelDisplayPipe, NMXDisplayMachineM : IOAccelDisplayMachine. newSurface/newDisplayPipe/
  newDisplayMachine return them when nmxmodern=1 (kernel log "display classes MODERN"). Default = legacy, unchanged.
- Shared code: surface swap/shape bodies are nv_surface_swap / nv_surface_shape; the pipe's initFramebufferResource,
  iopMapSurface, iopDropSources, performTransaction are templates (nvpipe_*) used by both pipes. fDM is IOAccelDisplayMachine*.
- Success signal: armed, nmx-ws-check.sh shows iop_ok rising ("performTransaction on the MODERN pipe" in the kernel log).
- Test with the normal config + nmxmodern=1 (out/config/config-modern.plist), arm by hand with nmx-ws-arm.sh.
