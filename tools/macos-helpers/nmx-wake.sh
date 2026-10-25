#!/bin/bash
# nmx-wake.sh (b131) - what the graphics path does AFTER a wake (picture black, only the cursor shows).
# Sleeps the Mac, notices the wake by itself, waits 30 s, then records: frames WindowServer still hands us, copy/flip
# counters, the driver's resume messages, WindowServer's display messages. The screen may stay black - that is fine:
# after the wake just wait 60 s, then hold the power button to restart. The report is saved on disk.
#   sudo ./nmx-wake.sh      Report (after the restart): /Users/Shared/nmx-wake.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-wake.txt
g() { sysctl -n debug.$1 2>/dev/null || echo 0; }
snap() { echo "ok=$(g nvaccel_iop_ok) fail=$(g nvaccel_iop_fail) empty=$(g nvaccel_iop_empty) blank=$(g nvaccel_iop_blank) copies=$(g nvaccel_iop_copies) settles=$(g nvaccel_iop_settles) dbflips=$(g nvaccel_iop_dbflips) flipdb=$(g nvaccel_iop_flipdb)"; }
W0=$(sysctl -n kern.waketime | sed 's/.*sec = \([0-9]*\).*/\1/')
T0=$(date '+%Y-%m-%d %H:%M:%S')
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)   nmx-fbpm: $(nvram nmx-fbpm 2>&1 | awk '{print $2}')"
  echo "WindowServer pid before: $(pgrep -x WindowServer)"
  echo "before sleep:      $(snap)"; } > $O
sync
echo "Sleeping in 5 s - hands off. Wake it with the power button after ~20 s, then just wait (60 s), then restart."
sleep 5
pmset sleepnow >/dev/null
while [ "$(sysctl -n kern.waketime | sed 's/.*sec = \([0-9]*\).*/\1/')" = "$W0" ]; do sleep 1; done
{ echo "woke at $(date '+%H:%M:%S')"; echo "right after wake:  $(snap)"; } >> $O; sync
sleep 15; echo "15 s after wake:   $(snap)" >> $O; sync
sleep 15
{ echo "30 s after wake:   $(snap)"
  echo "WindowServer pid after: $(pgrep -x WindowServer)"
  echo "--- driver + WindowServer messages since the sleep"
  log show --style compact --start "$T0" --predicate '(sender == "NMXGPU" OR sender == "NMXAccel") AND NOT eventMessage CONTAINS "NullMoth step"' 2>/dev/null | grep -v 'DISP\[' | head -150
  echo "--- WindowServer display messages"
  log show --style compact --start "$T0" --predicate 'process == "WindowServer" AND (eventMessage CONTAINS[c] "display" OR eventMessage CONTAINS[c] "wake" OR eventMessage CONTAINS[c] "error" OR eventMessage CONTAINS[c] "fail" OR eventMessage CONTAINS[c] "metal")' 2>/dev/null | head -80
  echo "--- driver breadcrumbs since the sleep"
  log show --style compact --start "$T0" --predicate 'eventMessage CONTAINS "NullMoth step"' 2>/dev/null | tail -40
} >> $O 2>&1
chmod 644 $O; sync; echo "done - restart if the screen is black, then send me $O"
