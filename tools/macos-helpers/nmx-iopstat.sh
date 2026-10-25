#!/bin/bash
# nmx-iopstat.sh (b118) - how many frames reach the panel and what each costs. Takes the counters, waits 15 s while
# you ANIMATE (open/close Mission Control, scroll a long page, drag a window), takes them again and prints frames/s
# and the time the CPU spends copying each frame (the cost R2's page flip removes). Plus the driver's frame-source log.
#   sudo ./nmx-iopstat.sh      Report: /Users/Shared/nmx-iopstat.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-iopstat.txt
g() { sysctl -n debug.$1 2>/dev/null || echo 0; }
snap() { echo "$(g nvaccel_iop_ok) $(g nvaccel_iop_copies) $(g nvaccel_iop_copy_ns) $(g nvaccel_iop_fail) $(g nvaccel_iop_empty)"; }
read OK0 C0 NS0 F0 E0 <<<"$(snap)"
echo "ANIMATE NOW for 15 s: Mission Control on/off, scroll, drag windows..."
sleep 15
read OK1 C1 NS1 F1 E1 <<<"$(snap)"
W=$(pgrep -x WindowServer | head -1)
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "frames to the panel in 15 s: $((OK1-OK0))  ($(( (OK1-OK0)/15 )) per s)   refused: $((F1-F0))   empty: $((E1-E0))"
  if [ "$((C1-C0))" -gt 0 ]; then
    echo "copies timed: $((C1-C0))   average copy: $(( (NS1-NS0)/(C1-C0)/1000 )) us   CPU busy copying: $(( (NS1-NS0)/150000000 )) %"
  else echo "copies timed: 0 (not b118 NMXAccel, or no frames)"; fi
  echo "slowest copy since boot: $(( $(g nvaccel_iop_copy_max_ns)/1000 )) us"
  echo "WindowServer CPU now: $(ps -o %cpu= -p $W)"
  echo "--- counters"; sysctl debug 2>/dev/null | grep -E 'nvaccel_iop' 
  echo "--- frame-source log (this boot)"
  log show --last boot --style compact --predicate 'eventMessage CONTAINS "iop: K6" OR eventMessage CONTAINS "MODERN pipe"' 2>/dev/null | tail -30
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
