#!/bin/bash
# nmx-cd3.sh - nmx-cd2 showed WHY CoreDisplay refuses the GPU path: CoreDisplay::useIOP is switched off because
# GetSystemGPUVendors::gpuVendors = 0 (it found no Intel/AMD/NVIDIA/paravirt GPU). This copies CoreDisplay's code
# (about 2 MB around UseIOPresentment) out of the running WindowServer, 4 KB per call, so the exact way
# GetSystemGPUVendors detects a GPU can be read. Read-only.
#   sudo ./nmx-cd3.sh        (armed, ~20 s)      Report: /Users/Shared/nmx-cd3.txt.gz
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1)
[ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-cd3.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -x bufsize=64m -x switchrate=200hz -p "$W" -n '
int st; uint64_t f; uint64_t cur; uint64_t endp;
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && st == 0/ {
  st = 1; f = uregs[R_PC]; cur = f - 0xc0000; endp = f + 0x120000; printf("FUNC %x\n", f); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1 && cur < endp/ { this->a = cur; cur += 4096; self->a = this->a; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /self->a/ { printf("@%x\n", self->a); tracemem(copyin(self->a, 4096), 4096); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /self->a/ { self->a = 0; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1 && cur >= endp/ { st = 2; exit(0); }
tick-60s { printf("TIMEOUT at %x\n", cur); exit(0); }' 2>&1 | grep -v "error on enabled probe"
} > $O 2>&1
gzip -f $O; chmod 644 $O.gz
echo "done - send me $O.gz ($(ls -lh $O.gz | awk "{print \$5}"))"
