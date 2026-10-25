#!/bin/bash
# nmx-copyfwd-trace.sh - 8 s of dtrace while you type/move a window: answers two questions about the armed desktop.
#  1. WindowServer logs "copy_forward_region: copy forward failed (0xe00002c7)" ~30x a second (10-20). WHICH call
#     answers kIOReturnUnsupported? -> WindowServer's IOConnectCallMethod selectors that return it + who called them,
#     and the kernel functions (IOAcceleratorFamily2 / NMXAccel) that return it.
#  2. NMXAccel's frame line says "frames: 66, copied 33": half the swaps never reach the panel. Why? -> how often
#     nvAccelResHasContent says yes/no and how often the copy to the panel runs.
# -Z: a probe that does not exist on this system (an inlined function) is skipped instead of stopping the trace.
# Needs SIP off for dtrace (this Mac: "sip": "disabled"). Read-only: it changes nothing.
#   sudo ./nmx-copyfwd-trace.sh          Report: /Users/Shared/nmx-copyfwd.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
R=/Users/Shared/nmx-copyfwd.txt
ws=$(pgrep -x WindowServer | head -1); [ -n "$ws" ] || { echo "no WindowServer"; exit 1; }
[ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ] || { echo "STOP: not armed - run nmx-ws-arm.sh first"; exit 1; }
echo "Tracing 8 s - TYPE in TextEdit or MOVE a window the whole time"
{
echo "=== $(date) copy-forward trace, WindowServer $ws"
csrutil status 2>&1
dtrace -q -Z -p "$ws" -n '
pid$target::IOConnectCallMethod:entry { self->sel = arg1; self->in = 1; }
pid$target::IOConnectCallMethod:return /self->in && arg1 == 0xe00002c7/ { @unsup_sel[self->sel] = count(); }
pid$target::IOConnectCallMethod:return /self->in && arg1 == 0xe00002c7 && shown < 3/ { shown++; printf("unsupported selector %d from:\n", self->sel); ustack(12); }
pid$target::IOConnectCallMethod:return /self->in/ { self->in = 0; }
pid$target::IOConnectCallScalarMethod:entry { self->ssel = arg1; self->sin = 1; }
pid$target::IOConnectCallScalarMethod:return /self->sin && arg1 == 0xe00002c7/ { @unsup_scalar[self->ssel] = count(); }
pid$target::IOConnectCallScalarMethod:return /self->sin/ { self->sin = 0; }
pid$target::*copy_forward_region*:entry { @cf = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2::return /arg1 == 0xe00002c7/ { @kunsup[probefunc] = count(); }
fbt:com.nullmoth.NMXAccel::return /arg1 == 0xe00002c7/ { @kunsup[probefunc] = count(); }
fbt:com.nullmoth.NMXAccel:*nvAccelResHasContent*:return { @content[arg1 & 0xff] = count(); }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry { @swaps = count(); }
fbt:com.nullmoth.NMXAccel:*nvAccelCopyToScanout*:return { @copies[arg1 & 0xff] = count(); }
tick-8s {
  printf("\n--- WindowServer IOConnectCallMethod selectors answering 0xe00002c7 (selector: count)\n"); printa("  %d: %@d\n", @unsup_sel);
  printf("--- ... IOConnectCallScalarMethod selectors\n"); printa("  %d: %@d\n", @unsup_scalar);
  printf("--- copy_forward_region calls: "); printa("%@d\n", @cf);
  printf("--- kernel functions returning 0xe00002c7\n"); printa("  %s: %@d\n", @kunsup);
  printf("--- swaps (submitSwapCopy): "); printa("%@d\n", @swaps);
  printf("--- nvAccelResHasContent result (1 = has content): \n"); printa("  %d: %@d\n", @content);
  printf("--- copy to panel result (1 = copied): \n"); printa("  %d: %@d\n", @copies);
  exit(0); }' 2>&1
} > "$R"
chmod 644 "$R"
echo "Report for me: $R"; tail -25 "$R"
