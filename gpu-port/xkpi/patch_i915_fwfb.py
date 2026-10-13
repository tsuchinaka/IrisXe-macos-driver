#!/usr/bin/env python3
"""patch_i915_fwfb.py <i915 gt/intel_ggtt.c>: keep GEM and i915's error-capture slot out of the GGTT range the firmware
framebuffer is scanned out through.

The firmware (OpenCore GOP) identity-maps its framebuffer at GGTT page 0 onwards, and macOS keeps drawing into it. On a normal
boot i915 takes that framebuffer over early (intel_initial_plane_config) and pins it at its GGTT address, so nothing else can
land there. In the port that takeover does not happen, so init_ggtt() reserves its error-capture slot at address 0 and GEM binds
ring/context/status pages at the lowest free addresses: both overwrite the framebuffer's page-table entries and the panel scans
out scratch memory. The patch reserves the first 32 MiB as an unevictable node before anything else is placed there.
Idempotent; exits non-zero if the anchor line is not found, so a drm-kmod change cannot silently drop the fix."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_FWFB" in s:
    print("already patched"); sys.exit(0)
anchor = "\tmutex_init(&ggtt->error_mutex);\n"
if s.count(anchor) != 1:
    print("ERROR: anchor not found exactly once in", p); sys.exit(1)
add = anchor + '''\t/* NMX_FWFB: GGTT [0, 32 MiB) still maps the firmware framebuffer; nothing may be bound there */
\t{
\t\tstatic struct drm_mm_node nmx_fwfb_node;

\t\tmemset(&nmx_fwfb_node, 0, sizeof(nmx_fwfb_node));
\t\tnmx_fwfb_node.start = 0;
\t\tnmx_fwfb_node.size = min_t(u64, 32ull << 20, ggtt->vm.total / 4);
\t\tnmx_fwfb_node.color = I915_COLOR_UNEVICTABLE;
\t\tif (drm_mm_reserve_node(&ggtt->vm.mm, &nmx_fwfb_node))
\t\t\tdrm_dbg(&ggtt->vm.i915->drm, "NMX: could not reserve the firmware framebuffer range\\n");
\t}
'''
p.write_text(s.replace(anchor, add, 1))
print("patched", p)
