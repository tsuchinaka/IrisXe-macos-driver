#!/bin/bash
# nmx-cd5.sh v2 (per-framebuffer vendor check + its strings) - reads the code of CoreDisplay's GetSystemGPUVendors (the run-once block that decides gpuVendors = 0).
# That code ran once at start-up and is no longer in memory, so a breakpoint-free dtrace probe is placed on it first
# (this pulls the page back in), then it is copied 256 bytes at a time. Read-only.
#   sudo ./nmx-cd5.sh     (armed, ~15 s)     Report: /Users/Shared/nmx-cd5.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1)
[ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-cd5.txt
{ echo "=== $(date)"
dtrace -Z -q -x bufsize=16m -p "$W" -n '
int st; uint64_t f; uint64_t inv; uint64_t cur; uint64_t endp;
pid$target:CoreDisplay:*GetSystemGPUVendors*:entry, pid$target:CoreDisplay:*PUVendor*:entry { printf("ran %s\n", probefunc); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && st == 0/ {
  st = 1; f = uregs[R_PC]; inv = *(uint64_t*)copyin(0x7ff840e526c0 - 0x7ff8049adb40 + f + 0x10, 8);
  printf("block invoke = 0x%x  %A\n", inv, inv); cur = f + 0xd5000; endp = f + 0xd5200; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1 && cur < endp/ { self->a = cur; cur += 256; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /self->a/ { printf("@%x\n", self->a); tracemem(copyin(self->a, 256), 256); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /self->a/ { self->a = 0; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1 && cur >= endp/ { st = 3; cur = f + 0x118d00; endp = f + 0x119000; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 3 && cur < endp/ { self->a = cur; cur += 256; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 3 && self->a/ { printf("@%x\n", self->a); tracemem(copyin(self->a, 256), 256); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 3 && self->a/ { self->a = 0; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 3 && cur >= endp/ { st = 4; exit(0); }
tick-15s { printf("TIMEOUT\n"); exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
