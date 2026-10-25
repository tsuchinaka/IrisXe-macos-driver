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
\t\tmemset(&nmx_fwfb_node, 0, sizeof(nmx_fwfb_node));
\t\tnmx_fwfb_node.start = 0;
\t\tnmx_fwfb_node.size = min_t(u64, 32ull << 20, ggtt->vm.total / 4);
\t\tnmx_fwfb_node.color = I915_COLOR_UNEVICTABLE;
\t\tnmx_fwfb_size = nmx_fwfb_node.size;
\t\tif (drm_mm_reserve_node(&ggtt->vm.mm, &nmx_fwfb_node))
\t\t\tdrm_dbg(&ggtt->vm.i915->drm, "NMX: could not reserve the firmware framebuffer range\\n");
\t}
'''
s = s.replace(anchor, add, 1)
# 10-18: the node is file-scope so the initial-plane takeover (patch_i915_initial_fb.py) can hand the range to i915's own
# object for the firmware framebuffer, and put the reservation back if that takeover fails
_k = s.rindex("\n}\n", 0, s.index("NMX_FWFB")) + 3   # file scope, just before the function that reserves
s = s[:_k] + "\n/* NMX_FWFB */\nstruct drm_mm_node nmx_fwfb_node;\nstatic u64 nmx_fwfb_size;\n" + s[_k:]
s += """
void nmx_fwfb_ggtt_release(struct i915_ggtt *ggtt);
void nmx_fwfb_ggtt_rereserve(struct i915_ggtt *ggtt);

/* NMX_FWFB: let the initial-plane takeover bind i915's object for the firmware framebuffer at GGTT 0 */
void nmx_fwfb_ggtt_release(struct i915_ggtt *ggtt)
{
	mutex_lock(&ggtt->vm.mutex);
	if (drm_mm_node_allocated(&nmx_fwfb_node))
		drm_mm_remove_node(&nmx_fwfb_node);
	mutex_unlock(&ggtt->vm.mutex);
}

/* NMX_FWFB: the takeover failed - protect the range again (macOS keeps drawing its console there) */
void nmx_fwfb_ggtt_rereserve(struct i915_ggtt *ggtt)
{
	int err = 0;

	mutex_lock(&ggtt->vm.mutex);
	if (!drm_mm_node_allocated(&nmx_fwfb_node) && nmx_fwfb_size) {
		memset(&nmx_fwfb_node, 0, sizeof(nmx_fwfb_node));
		nmx_fwfb_node.start = 0;
		nmx_fwfb_node.size = nmx_fwfb_size;
		nmx_fwfb_node.color = I915_COLOR_UNEVICTABLE;
		err = drm_mm_reserve_node(&ggtt->vm.mm, &nmx_fwfb_node);
	}
	mutex_unlock(&ggtt->vm.mutex);
	printk("NullMoth fwfb: GGTT range reserved again (%d)\\n", err);
}
"""
p.write_text(s)
print("patched", p)
