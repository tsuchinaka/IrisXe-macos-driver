#!/usr/bin/env python3
"""patch_i915_nosagv.py <i915 display/skl_watermark.c>: b192 - i915 does not control SAGV / QGV points unless boot-arg
nmxsagv=1. Tahoe hung twice in a row inside "commit tail: SAGV post-plane" (nullmoth-last-step 139, 10-27): the PCODE
request that relaxes the memory QGV points never returned, and every following boot hung there as well. With SAGV
control off, intel_has_sagv() is false - as on a machine whose BIOS has SAGV disabled - and i915 sends no SAGV / QGV
PCODE requests at all. Idempotent; exits non-zero when the anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_NOSAGV" in s:
    print("already patched"); sys.exit(0)
i = s.find("intel_has_sagv(struct drm_i915_private")
if i < 0 or s.count("intel_has_sagv(struct drm_i915_private") != 1:
    print("ERROR: intel_has_sagv() definition not found exactly once"); sys.exit(1)
j = s.index("\n}\n", i)
body = s[i:j]
if body.count("\treturn HAS_SAGV(") != 1:
    print("ERROR: intel_has_sagv() return line not found"); sys.exit(1)
s = s[:i] + body.replace("\treturn HAS_SAGV(", "\treturn nmx_sagv_on() && /* NMX_NOSAGV b192 */ HAS_SAGV(", 1) + s[j:]
s = "/* NMX_NOSAGV */\nextern int nmx_sagv_on(void);\n" + s
p.write_text(s)
print("patched", p)
