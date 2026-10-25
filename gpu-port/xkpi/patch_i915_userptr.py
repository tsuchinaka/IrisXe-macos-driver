#!/usr/bin/env python3
"""patch_i915_userptr.py <i915 gem/i915_gem_userptr.c>: make userptr objects (VK_EXT_external_memory_host) work.

10-18 (Brave zero-copy): every vkAllocateMemory on an imported host pointer was refused, because drm-kmod builds the
userptr ioctl without CONFIG_MMU_NOTIFIER (it then answers -ENODEV). cc-i915.sh now defines CONFIG_MMU_NOTIFIER for
the i915 sources; the port's linux/mmu_notifier.h accepts interval notifiers (pages stay pinned, the sequence never
moves). One more thing differs: LinuxKPI's pin_user_pages_fast() goes to __get_user_pages_fast(), which pins nothing
in this port (it returns 0), and i915 loops "while (pinned < num_pages)" - for ever. Pin through lkpi_get_user_pages()
instead (the port's IOMemoryDescriptor wiring, released by the last unpin).
Idempotent; exits non-zero when the anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_USERPTR" in s:
    print("already patched"); sys.exit(0)
a = "\t\tret = pin_user_pages_fast(obj->userptr.ptr + pinned * PAGE_SIZE,\n"
if s.count(a) != 1:
    print("ERROR: pin anchor not found exactly once in", p); sys.exit(1)
s = s.replace(a, "\t\tret = nmx_userptr_pin(obj, obj->userptr.ptr + pinned * PAGE_SIZE,\n", 1)
# 10-18 (build 68): Brave's GPU process lost its context over and over ("GPU event segment submission failed") once
# zero-copy used userptr objects. One suspect: i915 refuses (-EFAULT) a submission from a task whose mm is not the
# creator's; in this port the mm record can be replaced while the process lives. Pin the creator's pages instead
# (by mm when it differs) and log every refusal so the real cause shows in the kernel log.
a = "\tif (obj->userptr.notifier.mm != current->mm)\n\t\treturn -EFAULT;\n"
if s.count(a) != 1:
    print("ERROR: mm check anchor not found in", p); sys.exit(1)
s = s.replace(a, "\tif (obj->userptr.notifier.mm != current->mm) {\n"
                 "\t\tstatic int said;\n"
                 "\t\tif (said++ < 8)\n"
                 "\t\t\tprintk(\"NullMoth userptr: submission from another mm (%p, object made in %p) - pinning from the calling process\\n\",\n"
                 "\t\t\t       current->mm, obj->userptr.notifier.mm);\n"
                 "\t}\n", 1)
a = "out:\n\tif (pvec) {\n\t\tunpin_user_pages(pvec, pinned);\n\t\tkvfree(pvec);\n\t}\n\n\treturn ret;\n}\n"
if s.count(a) != 1:
    print("ERROR: submit_init exit anchor not found in", p); sys.exit(1)
s = s.replace(a, "out:\n\tif (pvec) {\n\t\tunpin_user_pages(pvec, pinned);\n\t\tkvfree(pvec);\n\t}\n"
                 "\tif (ret) {\n\t\tstatic int said;\n\t\tif (said++ < 16)\n"
                 "\t\t\tprintk(\"NullMoth userptr: submit_init failed %d (ptr 0x%lx, %zu bytes, %d of %lu pages pinned)\\n\",\n"
                 "\t\t\t       ret, (unsigned long)obj->userptr.ptr, (size_t)obj->base.size, pinned, num_pages);\n\t}\n"
                 "\n\treturn ret;\n}\n", 1)
inc = '#include "i915_scatterlist.h"\n'
if s.count(inc) != 1:
    print("ERROR: include anchor not found in", p); sys.exit(1)
s = s.replace(inc, inc + "\n/* NMX_USERPTR: pins through the port's wiring (see patch_i915_userptr.py) */\n"
              "long lkpi_get_user_pages(unsigned long start, unsigned long nr_pages, unsigned int gup_flags, struct page **pages);\n"
              "struct drm_i915_gem_object;\n"
              "static long nmx_userptr_pin(struct drm_i915_gem_object *obj, unsigned long start, unsigned long nr, unsigned int flags,\n"
              "\t\t\t    struct page **pages);\n", 1)
s += """
/* NMX_USERPTR: pin from the calling process, or through the creator's mm when the caller's differs */
static long nmx_userptr_pin(struct drm_i915_gem_object *obj, unsigned long start, unsigned long nr, unsigned int flags,
			    struct page **pages)
{
	long r;
	/* build 71: always the calling process. A userptr object cannot leave the process that made it (no flink, no
	   dma-buf export), so the submitting thread's task owns the pages even when the port gave it another mm record;
	   pinning through the creator's mm (build 68) resolved to kernel_task and panicked in IOMemoryDescriptor */
	(void)obj;
	r = lkpi_get_user_pages(start, nr, flags, pages);
	if (r <= 0) {
		static int said;
		if (said++ < 16)
			printk("NullMoth userptr: pinning %lu pages at 0x%lx failed (%ld)\\n", nr, start, r);
		if (r == 0)
			r = -EFAULT;   /* never loop for ever on a pin that makes no progress */
	}
	return r;
}
"""
p.write_text(s)
print("patched", p)
