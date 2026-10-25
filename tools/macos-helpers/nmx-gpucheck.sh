#!/bin/bash
# nmx-gpucheck.sh - is WindowServer on the GPU path now? Counts, for 10 s in the running WindowServer: what
# UseIOPresentment answers, which compositor it asks for / gets, Metal-compositor and IOPresentment work, and in the
# kernel the display-pipe transactions. Read-only.   sudo ./nmx-gpucheck.sh    Report: /Users/Shared/nmx-gpucheck.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1); [ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-gpucheck.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
echo "iop_ok before: $(sysctl -n debug.nvaccel_iop_ok 2>/dev/null)"
dtrace -Z -q -p "$W" -n '
pid$target:CoreDisplay:*UseIOPresentment*:return { @r[probefunc, (arg1 & 0xff) ? "YES" : "no"] = count(); }
pid$target:SkyLight:WSGetCompositorForAccelerator:return { @c["WSGetCompositorForAccelerator", arg1 ? "got one" : "NONE"] = count(); }
pid$target:SkyLight:*CompositorMetal*:entry, pid$target:SkyLight:*CompositorSW*:entry,
pid$target:QuartzCore:*MetalContext*:entry, pid$target:QuartzCore:*SWContext*:entry,
pid$target:CoreDisplay:*IOPresentment*:entry, pid$target:IOPresentment::entry { @f[probemod, probefunc] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipe*ransaction*:entry { @k[probefunc] = count(); }
tick-10s {
  printf("--- UseIOPresentment answers\n"); printa("  %-45s %-4s %@d\n", @r);
  printf("--- compositor lookups\n"); printa("  %-45s %-8s %@d\n", @c);
  printf("--- kernel display-pipe transaction calls\n"); printa("  %-110s %@d\n", @k);
  trunc(@f, 60); printf("--- top 60 compositor / Metal / IOPresentment functions\n"); printa("  %-12s %-110s %@d\n", @f);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
echo "iop_ok after: $(sysctl -n debug.nvaccel_iop_ok 2>/dev/null)"
echo "--- WindowServer CPU %: $(ps -o %cpu= -p $W)"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
