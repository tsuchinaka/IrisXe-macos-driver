#!/bin/bash
# nmx-late.sh (b129) - are typed characters missing because the GPU finishes the frame AFTER it was shown?
# Takes the late-change counters, waits 30 s while you TYPE in Safari/Notes, then prints how many frames changed after
# they were already on the panel (and when). Read-only.     sudo ./nmx-late.sh      Report: /Users/Shared/nmx-late.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-late.txt
g() { sysctl -n debug.$1 2>/dev/null || echo 0; }
snap() { echo "$(g nvaccel_iop_ok) $(g nvaccel_iop_late40) $(g nvaccel_iop_late190) $(g nvaccel_iop_late440) $(g nvaccel_iop_late1040) $(g nvaccel_iop_settles) $(g nvaccel_iop_latechk_ns)"; }
read F0 A0 B0 C0 D0 S0 N0 <<<"$(snap)"
echo "TYPE NOW for 30 s in Safari's address bar or Notes (normal speed, pause now and then)..."
sleep 30
read F1 A1 B1 C1 D1 S1 N1 <<<"$(snap)"
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "frames shown: $((F1-F0))   settle copies: $((S1-S0))   flip: $(g nvaccel_iop_flipdb)"
  echo "frames that changed AFTER they were shown:  <40ms $((A1-A0))   40-190ms $((B1-B0))   190-440ms $((C1-C0))   440-1040ms $((D1-D0))"
  [ "$((F1-F0))" -gt 0 ] && echo "checksum cost per frame: $(( (N1-N0)/(F1-F0)/1000 )) us"
  echo "--- counters"; sysctl debug 2>/dev/null | grep -E 'nvaccel_iop_(late|settle)'
  echo "--- log"
  log show --last 10m --style compact --predicate 'eventMessage CONTAINS "b129"' 2>/dev/null | tail -15
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
