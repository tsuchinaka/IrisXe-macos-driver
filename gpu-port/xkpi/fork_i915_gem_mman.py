# fork_i915_gem_mman.py <drm-kmod> <out>: i915_gem_mman.c for macOS. The legacy whole-object mmap ioctl
# (DRM_I915_GEM_MMAP) maps a GEM object's shmem object with FreeBSD's VM map; on macOS userland maps buffers through
# I915_GEM_MMAP_OFFSET and the node's mmap ioctl, which Mesa uses on every GPU the port drives (Gen9+).
import pathlib, sys
src = pathlib.Path(sys.argv[1]) / "drivers/gpu/drm/i915/gem/i915_gem_mman.c"
s = src.read_text()
start = s.index("#else\n\terror = 0;\n\taddr = 0;\n")
end = s.index("out:\n#endif\n") + len("out:\n#endif\n")
assert s.count("#else\n\terror = 0;\n\taddr = 0;\n") == 1 and s.count("out:\n#endif\n") == 1
s = s[:start] + "#else\n\t/* macOS port: no FreeBSD VM map; see the fork script's header */\n\ti915_gem_object_put(obj);\n\treturn -ENODEV;\n#endif\n" + s[end:]
s = s.replace("#include <sys/resourcevar.h>\t/* for lim_cur_proc */\n", "", 1)
# MMAP_GTT_VERSION: drm-kmod answers 3 on FreeBSD releases without its VM support, so Mesa ANV took the legacy
# I915_GEM_MMAP path (ENODEV above) and vkCreateDevice failed with VK_ERROR_MEMORY_MAP_FAILED (Iris Xe 10-17).
# MMAP_OFFSET works on macOS through the node's mmap ioctl: answer 4.
import re
m = re.search(r"int i915_gem_mmap_gtt_version\(void\)\n\{.*?\n\}\n", s, re.S)
assert m, "i915_gem_mmap_gtt_version not found"
s = s[:m.start()] + "int i915_gem_mmap_gtt_version(void)\n{\n\t/* macOS port: MMAP_OFFSET through the node's mmap ioctl */\n\treturn 4;\n}\n" + s[m.end():]
pathlib.Path(sys.argv[2]).write_text(s)
print("i915_gem_mman.c fork applied")
