#!/usr/bin/env python3
"""patch_i915_display_rpm.py <i915 display/intel_display_power.c>: display power checks must not ask runtime PM.

LinuxKPI stubs pm_runtime_get_if_in_use() to -EINVAL, so intel_runtime_pm_get_if_in_use() always fails and
intel_display_power_get_if_enabled() answered "off" for every domain: the boot readout saw the firmware's lit pipe as
disabled, skipped the framebuffer takeover and switched the panel's DDI off (10 s of black). This port never
runtime-suspends the GPU, so take the wakeref without asking (get_noresume).
Scoped to the display on purpose (10-18): answering "in use" for every caller (a pm_runtime.h override, build 58) also
woke GT paths that had never run here (TLB invalidation on unbind, RPS, fencing...) and mtlwindow hung after 8 s.
Idempotent; exits non-zero when the anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_DISPLAY_RPM" in s:
    print("already patched"); sys.exit(0)
a = "\twakeref = intel_runtime_pm_get_if_in_use(&dev_priv->runtime_pm);\n\tif (!wakeref)\n\t\treturn NULL;\n\n\tmutex_lock(&power_domains->lock);\n"
if s.count(a) != 1:
    print("ERROR: anchor not found exactly once in", p); sys.exit(1)
s = s.replace(a, "\t/* NMX_DISPLAY_RPM: the port never runtime-suspends; LinuxKPI's get_if_in_use stub always fails */\n"
              "\twakeref = intel_runtime_pm_get_noresume(&dev_priv->runtime_pm);\n\tif (!wakeref)\n\t\treturn NULL;\n\n\tmutex_lock(&power_domains->lock);\n", 1)
p.write_text(s)
print("patched", p)
