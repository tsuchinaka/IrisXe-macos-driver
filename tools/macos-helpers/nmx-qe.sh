#!/bin/bash
# nmx-qe.sh - why the Dock animations and the blur (transparency) are off while armed. macOS turns them off when it
# thinks the display is NOT accelerated ("no Quartz Extreme"), or when Reduce transparency / Reduce motion is on.
# Prints what macOS answers for the display, the accessibility settings, and which "is accelerated" checks WindowServer
# makes (and their answers) for 15 s while you hover over the Dock and open Launchpad. Read-only.
#   sudo ./nmx-qe.sh       Report: /Users/Shared/nmx-qe.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-qe.txt
U=$(stat -f %Su /dev/console)
W=$(pgrep -x WindowServer | head -1)
echo "HOVER over the Dock, click a Dock app, open Launchpad, for 15 s..."
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)  console user: $U"
  echo "--- what macOS answers for the main display (run as $U)"
  sudo -u "$U" osascript -l JavaScript -e '
ObjC.import("CoreGraphics");
var d = $.CGMainDisplayID();
"display " + d + "  CGDisplayUsesOpenGLAcceleration = " + $.CGDisplayUsesOpenGLAcceleration(d) +
"  CGDisplayIsBuiltin = " + $.CGDisplayIsBuiltin(d);' 2>&1
  echo "--- accessibility (1 = on)"
  for k in reduceTransparency reduceMotion increaseContrast; do
    echo "  $k: $(sudo -u "$U" defaults read com.apple.universalaccess $k 2>&1 | head -1)"; done
  echo "  Dock: magnification $(sudo -u "$U" defaults read com.apple.dock magnification 2>&1 | head -1), mineffect $(sudo -u "$U" defaults read com.apple.dock mineffect 2>&1 | head -1), launchanim $(sudo -u "$U" defaults read com.apple.dock launchanim 2>&1 | head -1)"
  echo "--- 'is it accelerated' checks inside WindowServer (module, function, return value, count)"
  dtrace -Z -q -p "$W" -n '
pid$target:SkyLight:*OpenGL*:return, pid$target:SkyLight:*QuartzExtreme*:return, pid$target:SkyLight:*Accelerat*:return,
pid$target:SkyLight:*Blur*:return, pid$target:SkyLight:*Backdrop*nabled*:return, pid$target:SkyLight:*Transparen*:return,
pid$target:CoreDisplay:*OpenGL*:return, pid$target:CoreDisplay:*AcceleratorBacked*:return, pid$target:CoreDisplay:*Accelerat*:return,
pid$target:QuartzCore:*backdrop*nabled*:return, pid$target:QuartzCore:*supports_backdrop*:return, pid$target:QuartzCore:*can_blur*:return
{ @r[probemod, probefunc, arg1] = count(); }
tick-15s { printa("  %-12s %-100s 0x%x %@d\n", @r); exit(0); }' 2>&1 | grep -v "error on enabled probe" | sort -k4 -n -r | head -80
  echo "--- display properties"
  ioreg -r -c IOFramebuffer -d 1 -l -w0 2>/dev/null | grep -E '"IOFB|"IOAccel|GLAccel|Accelerat|"IOGraphics|AAPL' | head -40
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
