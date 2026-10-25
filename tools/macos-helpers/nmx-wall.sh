#!/bin/bash
# nmx-wall.sh - why the Sequoia / Macintosh dynamic wallpapers do not show. Run it right AFTER selecting one of them in
# System Settings > Wallpaper. Collects the last 5 minutes of wallpaper agent / extension log messages (including the
# Metal driver's own messages from those processes) and any wallpaper crash reports. Read-only.
#   sudo ./nmx-wall.sh     Report: /Users/Shared/nmx-wall.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-wall.txt
U=$(stat -f %Su /dev/console)
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "--- wallpaper processes running"
  ps axo pid,%cpu,rss,command | grep -iE 'wallpaper|Aerial|Macintosh' | grep -v grep
  echo "--- wallpaper log, last 5 min (errors / faults first)"
  log show --last 5m --style compact --predicate 'process CONTAINS[c] "wallpaper" OR subsystem CONTAINS[c] "wallpaper"' 2>/dev/null \
    | grep -iE "error|fail|fault|metal|nvmtl|shader|pipeline|unsupported|crash|invalid|nil|refus" | tail -120
  echo "--- wallpaper log, last 5 min (everything, last 150 lines)"
  log show --last 5m --style compact --predicate 'process CONTAINS[c] "wallpaper" OR subsystem CONTAINS[c] "wallpaper"' 2>/dev/null | tail -150
  echo "--- crash reports"
  for d in /Library/Logs/DiagnosticReports "/Users/$U/Library/Logs/DiagnosticReports"; do
    ls -lt "$d" 2>/dev/null | grep -iE 'wallpaper|Macintosh|Sequoia|Aerial' | head -10
  done
  F=$(ls -t /Library/Logs/DiagnosticReports/*allpaper* "/Users/$U/Library/Logs/DiagnosticReports/"*allpaper* 2>/dev/null | head -1)
  [ -n "$F" ] && { echo "--- newest: $F"; head -c 12000 "$F"; }
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
