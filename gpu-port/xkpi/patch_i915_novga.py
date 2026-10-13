#!/usr/bin/env python3
"""patch_i915_novga.py <i915 display/intel_display_driver.c>: boot-arg nmxnovga=1 skips intel_vga_disable().

Experiment: i915 switches the legacy VGA plane off at start-up (VGA sequencer write plus VGA_CONTROL). macOS's own boot
framebuffer shares the device; with this knob the test can show whether that step changes what the framebuffer reports.
Idempotent; exits non-zero when the anchor is missing."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_NOVGA" in s:
    print("already patched"); sys.exit(0)
anchor = "\tintel_vga_disable(display);\n\tintel_setup_outputs(i915);\n"
if s.count(anchor) != 1:
    print("ERROR: anchor not found exactly once in", p); sys.exit(1)
new = "\t/* NMX_NOVGA: boot-arg nmxnovga=1 leaves the legacy VGA plane alone */\n\t{\n\t\textern int nmx_novga(void);\n\t\tif (!nmx_novga())\n\t\t\tintel_vga_disable(display);\n\t}\n\tintel_setup_outputs(i915);\n"
p.write_text(s.replace(anchor, new, 1))
print("patched", p)
