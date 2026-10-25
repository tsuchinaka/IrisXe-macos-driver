#!/bin/bash
# nmx-cd6.sh - prints the 4 words CoreDisplay searches for (case-insensitive) in each framebuffer's CLASS NAME to decide
# its GPU vendor (2 Intel, 4 AMD, 16 paravirt, 8 NVIDIA), and the function it gets the name with. Read-only, ~2 s.
#   sudo ./nmx-cd6.sh      Report: /Users/Shared/nmx-cd6.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1); [ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-cd6.txt
{ echo "=== $(date)"
dtrace -q -p "$W" -n '
int st; uint64_t f; uint64_t s;
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && st == 0/ { st = 1; f = uregs[R_PC]; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { s = f + 0x3c4aec78; printf("vendor 0x02 word: %s\n", copyinstr(*(uint64_t*)copyin(s + 16, 8))); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { s = f + 0x3c4aac58; printf("vendor 0x04 word: %s\n", copyinstr(*(uint64_t*)copyin(s + 16, 8))); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { s = f + 0x3c4aec98; printf("vendor 0x10 word: %s\n", copyinstr(*(uint64_t*)copyin(s + 16, 8))); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { s = f + 0x3c4a7158; printf("vendor 0x08 word: %s\n", copyinstr(*(uint64_t*)copyin(s + 16, 8))); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { s = f + 0xe90b8; printf("name function: %A\n", *(uint64_t*)copyin(s + 6 + *(int32_t*)copyin(s + 2, 4), 8)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /st == 1/ { st = 2; exit(0); }
tick-10s { exit(0); }' 2>&1 | grep -v "error on enabled probe"
echo "--- framebuffer classes"; ioreg -r -c IOFramebuffer -d 1 -w0 | grep -- '+-o' | head
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
