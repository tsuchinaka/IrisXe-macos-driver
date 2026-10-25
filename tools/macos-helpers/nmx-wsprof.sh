#!/bin/bash
# nmx-wsprof.sh - where WindowServer's CPU goes while animating (58 % with b118, 28 frames/s). Samples WindowServer
# ~1000 times a second for 15 s: time per library, top functions, top call stacks, and kernel time. Read-only.
#   sudo ./nmx-wsprof.sh   then ANIMATE for 15 s (Mission Control on/off, scroll, drag windows)
#   Report: /Users/Shared/nmx-wsprof.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1); [ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-wsprof.txt
echo "ANIMATE NOW for 15 s: Mission Control on/off, scroll, drag windows..."
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -x ustackframes=12 -x bufsize=32m -p "$W" -n '
profile-997 /pid == $target && arg1/ { @mod[umod(arg1)] = count(); @fn[ufunc(arg1)] = count(); @st[ustack(8)] = count(); @all["user"] = count(); }
profile-997 /pid == $target && arg0/ { @kfn[func(arg0)] = count(); @all["kernel"] = count(); }
profile-997 /pid == $target/ { @thr[tid] = count(); }
tick-15s {
  printf("--- samples (997/s per CPU; 15 s)\n"); printa("  %-8s %@d\n", @all);
  trunc(@mod, 25); printf("\n--- by library\n"); printa("  %-50A %@d\n", @mod);
  trunc(@fn, 50); printf("\n--- top functions\n"); printa("  %-110A %@d\n", @fn);
  trunc(@kfn, 30); printf("\n--- top kernel functions\n"); printa("  %-90a %@d\n", @kfn);
  trunc(@thr, 10); printf("\n--- busiest threads (tid)\n"); printa("  %d %@d\n", @thr);
  trunc(@st, 25); printf("\n--- top 25 call stacks\n"); printa("%k\n   %@d samples\n\n", @st);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
