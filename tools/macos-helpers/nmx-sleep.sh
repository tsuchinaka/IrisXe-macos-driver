#!/bin/bash
# nmx-sleep.sh - what happened at the last sleep / wake, and where the brightness slider gets its value. Read-only.
# Run it right after a failed sleep/wake (after the forced restart if it hung).   sudo ./nmx-sleep.sh
# Report: /Users/Shared/nmx-sleep.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-sleep.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "--- driver's last breadcrumb (where it was when it stopped)"
  nvram nullmoth-last-step 2>&1; nvram nullmoth-gpu-attaching 2>&1; nvram nmintelfb-brightness 2>&1
  echo "--- newest panic files"
  ls -lt /Library/Logs/DiagnosticReports/*.panic 2>/dev/null | head -3
  echo "--- sleep settings"; pmset -g 2>&1 | head -30
  echo "--- last sleep / wake events"
  pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Start|Failure|Assertions) ' | tail -25
  echo "--- sleep/wake failure info"; sysctl kern.sleeptime kern.waketime kern.wakereason 2>/dev/null
  echo "--- driver power + backlight messages (this boot)"
  log show --last boot --style compact --predicate 'eventMessage CONTAINS "NullMoth" AND (eventMessage CONTAINS[c] "power" OR eventMessage CONTAINS[c] "backlight" OR eventMessage CONTAINS[c] "suspend" OR eventMessage CONTAINS[c] "resume" OR eventMessage CONTAINS[c] "DPMS" OR eventMessage CONTAINS[c] "sleep" OR eventMessage CONTAINS[c] "wake")' 2>/dev/null | tail -60
  echo "--- previous boot (if it hung on sleep/wake)"
  log show --style compact --start "$(date -v-2H '+%Y-%m-%d %H:%M:%S')" --predicate 'eventMessage CONTAINS "NullMoth" AND (eventMessage CONTAINS[c] "power" OR eventMessage CONTAINS[c] "suspend" OR eventMessage CONTAINS[c] "resume" OR eventMessage CONTAINS[c] "sleep")' 2>/dev/null | tail -40
  echo "--- brightness as macOS sees it"
  ioreg -lw0 -r -c IODisplayConnect 2>/dev/null | grep -E 'IODisplayParameters|brightness' | head -10
  ioreg -lw0 -r -c IODisplay 2>/dev/null | grep -oE '"brightness"=\{[^}]*\}' | head -3
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
