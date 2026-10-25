#!/bin/bash
# nmx-unlock.sh - what the Mac does during the ~10 s freeze after unlocking from the lock screen. Samples every process
# (CPU time per process, WindowServer's call stacks, kernel functions) for 45 s. Read-only.
#   sudo ./nmx-unlock.sh   -> within 10 s lock the screen (Ctrl+Cmd+Q), wait 5 s, unlock, wait until it is smooth.
#   Report: /Users/Shared/nmx-unlock.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-unlock.txt
echo "Recording 45 s: NOW lock the screen (Ctrl+Cmd+Q), wait ~5 s, log in again, wait until it is smooth."
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -x ustackframes=12 -x bufsize=64m -n '
profile-997 { @proc[execname] = count(); }
profile-997 /arg0/ { @kfn[func(arg0)] = count(); }
profile-997 /execname == "WindowServer" && arg1/ { @wsfn[ufunc(arg1)] = count(); @wsst[ustack(10)] = count(); }
profile-997 /execname == "loginwindow" && arg1/ { @lwfn[ufunc(arg1)] = count(); }
sched:::off-cpu /execname == "WindowServer"/ { self->ts = timestamp; }
sched:::on-cpu /self->ts && timestamp - self->ts > 200000000/ { @blocked[ustack(10)] = quantize((timestamp - self->ts) / 1000000); }
sched:::on-cpu /self->ts/ { self->ts = 0; }
tick-1s { @persec[walltimestamp / 1000000000] = sum(0); }
tick-45s {
  trunc(@proc, 15); printf("--- CPU samples per process (45 s)\n"); printa("  %-24s %@d\n", @proc);
  trunc(@wsfn, 40); printf("\n--- WindowServer top functions\n"); printa("  %-110A %@d\n", @wsfn);
  trunc(@lwfn, 20); printf("\n--- loginwindow top functions\n"); printa("  %-110A %@d\n", @lwfn);
  trunc(@kfn, 30); printf("\n--- kernel top functions\n"); printa("  %-90a %@d\n", @kfn);
  printf("\n--- WindowServer threads that were BLOCKED more than 200 ms (where they waited, ms)\n"); printa("%k\n%@d\n", @blocked);
  trunc(@wsst, 15); printf("\n--- WindowServer top stacks\n"); printa("%k\n   %@d samples\n\n", @wsst);
  exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
