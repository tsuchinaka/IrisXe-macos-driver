#!/usr/bin/env python3
"""patch_i915_stolen_fwfb.py <i915 gem/i915_gem_stolen.c>: keep i915's stolen-memory allocator away from the firmware framebuffer.

The firmware framebuffer lives at the start of the graphics stolen memory, and the port hands i915 the whole stolen range. i915
allocates ring buffers, contexts and other objects from stolen memory and clears them, starting at offset 0, so it overwrites
the picture macOS draws into. A normal boot avoids this because i915 first takes the framebuffer over (intel_initial_plane_config)
and holds that range; that takeover does not happen in the port. The patch reserves the first 32 MiB of the allocator.
Idempotent; exits non-zero when the anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_FWFB_STOLEN" in s:
    print("already patched"); sys.exit(0)
anchor = "\tdrm_mm_init(&i915->mm.stolen, 0, i915->dsm.usable_size);\n"
if s.count(anchor) != 1:
    print("ERROR: anchor not found exactly once in", p); sys.exit(1)
add = anchor + '''\t/* NMX_FWFB_STOLEN: the firmware framebuffer is at offset 0 of stolen memory; the allocator must not hand it out */
\t{
\t\tstatic struct drm_mm_node nmx_fwfb_stolen;

\t\tmemset(&nmx_fwfb_stolen, 0, sizeof(nmx_fwfb_stolen));
\t\tnmx_fwfb_stolen.start = 0;
\t\tnmx_fwfb_stolen.size = min_t(u64, 32ull << 20, i915->dsm.usable_size / 2);
\t\tif (drm_mm_reserve_node(&i915->mm.stolen, &nmx_fwfb_stolen))
\t\t\tdrm_dbg(&i915->drm, "NMX: could not reserve the firmware framebuffer in stolen memory\\n");
\t}
'''
p.write_text(s.replace(anchor, add, 1))
print("patched", p)
