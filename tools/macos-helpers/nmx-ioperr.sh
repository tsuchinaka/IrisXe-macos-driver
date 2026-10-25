#!/bin/bash
# nmx-ioperr.sh - which IOPresentment errors WindowServer hits (about 100 per 10 s with b117) and where they come
# from: error code + the 8 calling functions, and the kernel display-pipe calls that returned an error. Read-only, 10 s.
#   sudo ./nmx-ioperr.sh      (move a window / type while it runs)     Report: /Users/Shared/nmx-ioperr.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1); [ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-ioperr.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -Z -q -p "$W" -n '
pid$target:IOPresentment:*IOPresentmentErrorSetErrorCode*:entry { @e[arg1, ustack(8)] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipe*:return /arg1 != 0 && arg1 < 0x100000000 && (arg1 & 0xe0000000) == 0xe0000000/
  { @k[probefunc, arg1] = count(); }
tick-10s {
  printf("--- IOPresentment error codes (code, caller stack, count)\n"); printa("code 0x%x\n%k\n   count %@d\n\n", @e);
  printf("--- kernel display-pipe calls that returned an IOReturn error (function, error, count)\n"); printa("  %-100s 0x%x %@d\n", @k);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
