#!/usr/bin/env python3
"""patch_i915_initial_fb.py <i915 display/intel_plane_initial.c>: let i915 take the firmware framebuffer over at boot.

10-18: with the readout fixed (override/include/linux/pm_runtime.h) i915 sees the firmware's lit pipe and tries to wrap
the firmware framebuffer (stolen offset 0, GGTT 0) in its own object, so the panel keeps its picture through the
driver start instead of going black. patch_i915_fwfb.py and patch_i915_stolen_fwfb.py fence that range off; this hands
the range to the takeover, puts the fence back if the takeover fails, and on success keeps the object pinned for good:
macOS keeps drawing its console into those pages, so nothing else may ever be placed there.
Idempotent; exits non-zero when an anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_INITIAL_FB" in s:
    print("already patched"); sys.exit(0)

def sub(old, new):
    global s
    if s.count(old) != 1:
        print("ERROR: anchor not found exactly once:", repr(old[:70])); sys.exit(1)
    s = s.replace(old, new, 1)

decl = '''/* NMX_INITIAL_FB */
struct i915_ggtt;
void nmx_fwfb_ggtt_release(struct i915_ggtt *ggtt);
void nmx_fwfb_ggtt_rereserve(struct i915_ggtt *ggtt);
void nmx_fwfb_stolen_release(struct drm_i915_private *i915);
void nmx_fwfb_stolen_rereserve(struct drm_i915_private *i915);

static void nmx_fwfb_fence_back(struct drm_i915_private *i915)
{
	i915_gem_drain_freed_objects(i915);
	nmx_fwfb_stolen_rereserve(i915);
	nmx_fwfb_ggtt_rereserve(to_gt(i915)->ggtt);
}

static struct i915_vma *
initial_plane_vma(struct intel_display *display,'''
sub("static struct i915_vma *\ninitial_plane_vma(struct intel_display *display,", decl)

sub("\tobj = i915_gem_object_create_region_at(mem, phys_base, size,\n",
    "\tprintk(\"NullMoth fwfb: taking the firmware framebuffer over (GGTT 0x%x, %u bytes)\\n\", plane_config->base, size);\n"
    "\tnmx_fwfb_stolen_release(i915);\n\tnmx_fwfb_ggtt_release(to_gt(i915)->ggtt);\n"
    "\tobj = i915_gem_object_create_region_at(mem, phys_base, size,\n")

sub("\t\tdrm_dbg_kms(display->drm, \"Failed to preallocate initial FB in %s\\n\",\n\t\t\t    mem->region.name);\n\t\treturn NULL;\n",
    "\t\tdrm_dbg_kms(display->drm, \"Failed to preallocate initial FB in %s\\n\",\n\t\t\t    mem->region.name);\n"
    "\t\tprintk(\"NullMoth fwfb: takeover failed: no object in %s (%ld)\\n\", mem->region.name, PTR_ERR(obj));\n"
    "\t\tnmx_fwfb_fence_back(i915);\n\t\treturn NULL;\n")

sub("\tdrm_dbg_kms(display->drm,\n\t\t    \"Initial plane fb bound to 0x%x in the ggtt (original 0x%x)\\n\",\n\t\t    i915_ggtt_offset(vma), plane_config->base);\n",
    "\tdrm_dbg_kms(display->drm,\n\t\t    \"Initial plane fb bound to 0x%x in the ggtt (original 0x%x)\\n\",\n\t\t    i915_ggtt_offset(vma), plane_config->base);\n"
    "\t/* keep it for good: the macOS console still draws into these pages */\n"
    "\ti915_gem_object_get(obj);\n\t__i915_vma_pin(vma);\n"
    "\tprintk(\"NullMoth fwfb: firmware framebuffer taken over at GGTT 0x%x\\n\", i915_ggtt_offset(vma));\n")

sub("err_obj:\n\tif (drm_mm_node_allocated(&orig_mm))\n\t\tdrm_mm_remove_node(&orig_mm);\n\ti915_gem_object_put(obj);\n\treturn NULL;\n",
    "err_obj:\n\tif (drm_mm_node_allocated(&orig_mm))\n\t\tdrm_mm_remove_node(&orig_mm);\n\ti915_gem_object_put(obj);\n"
    "\tprintk(\"NullMoth fwfb: takeover failed binding the object\\n\");\n"
    "\tnmx_fwfb_fence_back(i915);\n\treturn NULL;\n")

# the plane switched off because the takeover failed: say so (this is the black screen at boot)
sub("\tintel_plane_disable_noatomic(crtc, plane);\n\n\treturn;\n",
    "\tprintk(\"NullMoth fwfb: no takeover, switching the firmware's plane off\\n\");\n"
    "\tintel_plane_disable_noatomic(crtc, plane);\n\n\treturn;\n")

if '#include "i915_gem.h"' not in s:
    sub('#include "i915_drv.h"\n', '#include "i915_drv.h"\n#include "i915_gem.h"\n#include "gt/intel_gt.h"\n')
p.write_text(s)
print("patched", p)
