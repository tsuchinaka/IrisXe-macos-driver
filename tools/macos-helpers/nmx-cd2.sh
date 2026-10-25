#!/bin/bash
# nmx-cd2.sh - reads the switches CoreDisplay::UseIOPresentment() decides with, inside the running (armed) WindowServer.
# The function (from nmx-cd.txt) is:  if (!G0) return NO;  once{...};  bits = 2*B1 | 8*B2 | 4*B3 | 16*B4;
#                                      if (MASK && (bits & MASK) == MASK) return G0;  else { G0 = 0; return NO; }
# This prints G0, B1..B4, MASK, the names of those globals, the strings and the functions it calls.
# v2: one item per clause (a page that is not in memory only loses that item).
# Read-only. Run while armed:   sudo ./nmx-cd2.sh      Report: /Users/Shared/nmx-cd2.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
W=$(pgrep -x WindowServer | head -1)
[ -n "$W" ] || { echo "WindowServer not running"; exit 1; }
O=/Users/Shared/nmx-cd2.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -p "$W" -n '
int n; uint64_t f; uint64_t s;
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 0/ { n = 1; f = uregs[R_PC]; }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("function at 0x%x  %A\n", f, f); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("G0   (enable)  = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb54c2d, 1), f + 0x3fb54c2d); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("B1   (bit 2)   = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb54c2e, 1), f + 0x3fb54c2e); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("B2   (bit 8)   = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb55741, 1), f + 0x3fb55741); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("B3   (bit 4)   = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb55742, 1), f + 0x3fb55742); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("B4   (bit 16)  = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb55743, 1), f + 0x3fb55743); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("MASK (needed)  = 0x%x at %A\n", *(uint32_t*)copyin(f + 0x3fb5a224, 4), f + 0x3fb5a224); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("once           = 0x%x at %A\n", *(uint64_t*)copyin(f + 0x3fb5a228, 8), f + 0x3fb5a228); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("init flag      = %d   at %A\n", *(uint8_t*)copyin(f + 0x3fb55748, 1), f + 0x3fb55748); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("init: caps lo=0x%x hi=0x%x  flags f290=%d f2f1=%d f2f2=%d\n", *(uint32_t*)copyin(f+0x3fb5574c,4), *(uint32_t*)copyin(f+0x3fb54c3c,4), *(uint8_t*)copyin(f+0x3fb55750,1), *(uint8_t*)copyin(f+0x3fb557b1,1), *(uint8_t*)copyin(f+0x3fb557b2,1)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("block global   = 0x%x at %A\n", *(uint64_t*)copyin(f + 0x3fb55c88, 8), f + 0x3fb55c88); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("globals around G0:\n"); tracemem(copyin(f + 0x3fb54c20, 64), 64); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("globals around B2:\n"); tracemem(copyin(f + 0x3fb55730, 64), 64); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { s = f + 0xe96d0; printf("stub1 (once?)  -> %A\n", *(uint64_t*)copyin(s + 6 + *(int32_t*)copyin(s + 2, 4), 8)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { s = f + 0xe96ca; printf("stub2 (block)  -> %A\n", *(uint64_t*)copyin(s + 6 + *(int32_t*)copyin(s + 2, 4), 8)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { s = f + 0xe9910; printf("stub3 (block2) -> %A\n", *(uint64_t*)copyin(s + 6 + *(int32_t*)copyin(s + 2, 4), 8)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { s = f + 0xe98fe; printf("stub4 (init)   -> %A\n", *(uint64_t*)copyin(s + 6 + *(int32_t*)copyin(s + 2, 4), 8)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("str1 = %s\n", copyinstr(f + 0xfb9bd)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("str2 = %s\n", copyinstr(f + 0xfb947)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("str3 = %s\n", copyinstr(f + 0xfb98b)); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { printf("code before the function:\n"); tracemem(copyin(f - 1024, 1024), 1024); }
pid$target:CoreDisplay:*UseIOPresentment*:entry /probefunc == "CoreDisplay::UseIOPresentment()" && n == 1/ { n = 2; exit(0); }
tick-15s { printf("UseIOPresentment was not called in 15 s\n"); exit(0); }' 2>&1 | grep -v "error on enabled probe"
echo "--- CoreDisplay / IOPresentment preferences"
defaults read /Library/Preferences/com.apple.CoreDisplay 2>&1 | head -40
defaults read /Library/Preferences/com.apple.windowserver 2>&1 | grep -iE 'present|metal|compos' | head
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
