#!/bin/bash
# nmx-type-latency.sh - where does the typing delay go? 15 s of dtrace while you type in TextEdit (armed desktop).
# For every letter TextEdit inserts (-[NSTextView insertText:...]) it measures:
#   key -> commit  : TextEdit handled the key until it handed the new picture to WindowServer (CoreAnimation commit)
#   key -> swap    : ... until WindowServer's next frame reached NMXAccel (submitSwapCopy = the copy to the panel)
# and how evenly WindowServer delivers frames (gaps between swaps). Read-only (dtrace, SIP off).
#   1. open TextEdit with a document     2. sudo ./nmx-type-latency.sh     3. type steadily in TextEdit for 15 s
# Report: /Users/Shared/nmx-type-latency.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
R=/Users/Shared/nmx-type-latency.txt
te=$(pgrep -x TextEdit | head -1); [ -n "$te" ] || { echo "open TextEdit first"; exit 1; }
echo "Recording 15 s in 3 s - click into the TextEdit document and type steadily until it says done"
sleep 3; echo "NOW - type"
{
echo "=== $(date) typing latency, TextEdit $te, armed $(sysctl -n debug.nvaccelfb 2>/dev/null), boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -Z -p "$te" -n '
uint64_t key, last; int keys, swaps, waitc, waits;
objc$target:NSTextView:-insertText*:entry { key = timestamp; keys++; waitc = 1; waits = 1; }
pid$target:QuartzCore:*CA*Transaction*commit*:entry /waitc && key/ { @commit = quantize((timestamp - key) / 1000000); waitc = 0; }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry /waits && key/ { @swap = quantize((timestamp - key) / 1000000); @swapavg = avg((timestamp - key) / 1000000); @swapmax = max((timestamp - key) / 1000000); waits = 0; }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry /last/ { @gap = quantize((timestamp - last) / 1000000); }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry { last = timestamp; swaps++; }
tick-15s {
  printf("letters typed: %d   swaps: %d\n", keys, swaps);
  printf("\n--- key -> CoreAnimation commit in TextEdit (ms)\n"); printa(@commit);
  printf("\n--- key -> next frame at the panel copy (ms)\n"); printa(@swap); printa("average %@d ms, worst %@d ms\n", @swapavg, @swapmax);
  printf("\n--- gaps between frames (ms)\n"); printa(@gap);
  exit(0); }' 2>&1
echo "--- frame lines"; log show --last 1m --style compact --predicate 'eventMessage CONTAINS "frames:"' 2>/dev/null | tail -16
} > "$R"
chmod 644 "$R"
echo "done. Report for me: $R"
grep -E "letters typed|average" "$R"
