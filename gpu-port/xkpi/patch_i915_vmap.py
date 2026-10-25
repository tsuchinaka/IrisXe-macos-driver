#!/usr/bin/env python3
"""patch_i915_vmap.py <i915 gem/i915_gem_object.c>: give i915 GEM objects a CPU mapping for DRM clients.

The display bridge (nmx_fb_bridge.c) draws into a DRM client buffer through drm_client_buffer_vmap(). i915 objects have no
vmap hook (Linux's own fbdev maps them through the GGTT aperture instead), so the call fails with EOPNOTSUPP (102) and the
kext publishes no framebuffer. The hook added here pins the object into the mappable GGTT and returns the aperture mapping,
which is one contiguous physical range whatever pages back the object. DRM calls it with the object's reservation held.
Idempotent; exits non-zero when an anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_GEM_VMAP" in s:
    print("already patched"); sys.exit(0)
a1 = "static const struct drm_gem_object_funcs i915_gem_object_funcs = {\n"
a2 = "\t.export = i915_gem_prime_export,\n};"
if s.count(a1) != 1 or s.count(a2) != 1:
    print("ERROR: anchors not found exactly once in", p); sys.exit(1)
funcs = '''/* NMX_GEM_VMAP: CPU mapping of an object through the GGTT aperture, for the display bridge's DRM client buffer */
#include "i915_vma.h"
#include "i915_gem.h"
#include "gt/intel_gt.h"
#include "i915_gem_domain.h"
extern void nmx_step(const char *what);

static int nmx_gem_vmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct drm_i915_gem_object *obj = to_intel_bo(gem);
	struct i915_vma *vma;
	void __iomem *ptr;

	/* the caller holds the object's reservation, as the _ww variant requires */
	printk("NullMoth gem: vmap start, %zu bytes\\n", (size_t)gem->size);
	nmx_step("vmap: start");
	/*
	 * Set the cache level the display plane will ask for BEFORE pinning. Otherwise the display pin calls
	 * i915_gem_object_set_cache_level(), which must unbind every vma of the object; our pinned vma answers -EAGAIN
	 * and i915_gem_object_unbind(..., UNBIND_BARRIER) retries forever (the attach hung in "prepare planes").
	 */
	{
		int r = i915_gem_object_set_cache_level(obj, HAS_WT(to_i915(gem->dev)) ? I915_CACHE_WT : I915_CACHE_NONE);
		printk("NullMoth gem: vmap: display cache level set (%d)\\n", r);
		nmx_step(r ? "vmap: cache level FAILED" : "vmap: cache level set");
		if (r)
			return r;
	}
	/* 256 KiB covers Gen12's 4 KiB linear scan-out alignment and the 256 KiB VT-d case, so the plane pin can reuse this vma */
	vma = i915_gem_object_ggtt_pin_ww(obj, NULL, NULL, 0, 256 * 1024, PIN_MAPPABLE);
	if (IS_ERR(vma)) {
		printk("NullMoth gem: vmap: pinning into the aperture failed (%ld)\\n", PTR_ERR(vma));
		return PTR_ERR(vma);
	}
	printk("NullMoth gem: vmap: pinned at 0x%llx\\n", (unsigned long long)i915_ggtt_offset(vma));
	ptr = i915_vma_pin_iomap(vma);
	if (IS_ERR(ptr)) {
		printk("NullMoth gem: vmap: aperture mapping failed (%ld)\\n", PTR_ERR(ptr));
		i915_vma_unpin(vma);
		return PTR_ERR(ptr);
	}
	printk("NullMoth gem: vmap: mapped\\n");
	nmx_step("vmap: mapped");
	iosys_map_set_vaddr_iomem(map, ptr);
	return 0;
}

static void nmx_gem_vunmap(struct drm_gem_object *gem, struct iosys_map *map)
{
	struct drm_i915_gem_object *obj = to_intel_bo(gem);
	struct i915_ggtt *ggtt = to_gt(to_i915(gem->dev))->ggtt;
	struct i915_vma *vma = i915_vma_instance(obj, &ggtt->vm, NULL);

	if (!IS_ERR(vma)) {
		i915_vma_unpin_iomap(vma);
		i915_vma_unpin(vma);
	}
}

'''
s = s.replace(a1, funcs + a1, 1)
s = s.replace(a2, "\t.export = i915_gem_prime_export,\n\t.vmap = nmx_gem_vmap,\n\t.vunmap = nmx_gem_vunmap,\n};", 1)
p.write_text(s)
print("patched", p)
