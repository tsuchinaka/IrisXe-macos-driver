#!/bin/bash
# nmx-wakecheck.sh [minutes] - what happened during the last sleeps and wakes (run it AFTER the test, back at the desktop).
#
#   sudo ./nmx-wakecheck.sh 30      -> looks at the last 30 minutes (default), writes ~/Desktop/nmx-wakecheck.txt
#
# Changes nothing. Collects: the sleep / wake list from powerd (with wake reasons and failures), the graphics driver's
# messages around them (hand-over, PSR on/off, flips, wake fix, errors), whether WindowServer restarted or crashed,
# Wi-Fi state now, and any kernel panic report since the test started.
MIN=${1:-30}
OUT="$HOME/Desktop/nmx-wakecheck.txt"
[ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/nmx-wakecheck.txt"
{
echo "=== wake check $(date), last $MIN min - boot-args: $(sysctl -n kern.bootargs)"
echo
echo "--- sleeps and wakes (powerd)"
pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Failure|Kernel Client Acks|Sleep Service|Wake Requests) ' | tail -30 | cut -c1-230
echo
echo "--- graphics driver around sleep / wake"
log show --last "${MIN}m" --style compact --predicate '(sender == "NMXGPU" OR sender == "NMXAccel") AND NOT eventMessage CONTAINS "NullMoth step" AND NOT eventMessage CONTAINS "clock governor" AND NOT eventMessage CONTAINS "VBL:"' 2>/dev/null \
  | grep -iE 'power|sleep|wake|dpms|self refresh|PSR|flip|hand-over|backlight|error|fail|refused|timeout|hang|reset' | tail -60 | cut -c1-230
echo
echo "--- WindowServer"
for p in $(pgrep -x WindowServer); do echo "pid $p running for $(ps -o etime= -p "$p" | tr -d ' ')"; done
log show --last "${MIN}m" --style compact --predicate 'process == "WindowServer"' 2>/dev/null \
  | grep -iE 'display sleep|did wake|wake|transaction submission failed|timed out|crash|fatal' | tail -15 | cut -c1-230
ls -t /Library/Logs/DiagnosticReports/ 2>/dev/null | grep -iE 'WindowServer|panic' | head -5 | sed 's/^/report: /'
echo
echo "--- acceleration fix-ups after wake"
sysctl debug.nvaccel_wakefixes debug.nvaccelfb 2>/dev/null
echo
echo "--- Wi-Fi now"
W=$(networksetup -listallhardwareports 2>/dev/null | awk '/Wi-Fi|AirPort/{getline; print $2; exit}')
echo "device ${W:-?}: $(ifconfig "${W:-en0}" 2>/dev/null | awk '/status:/{print $2}'), address $(ipconfig getifaddr "${W:-en0}" 2>/dev/null || echo none)"
log show --last "${MIN}m" --style compact --predicate 'sender == "AirportItlwm" OR process == "airportd"' 2>/dev/null \
  | grep -iE 'wake|sleep|fail|error|firmware|reset|power' | tail -10 | cut -c1-200
echo
echo "--- kernel panics since the test"
find /Library/Logs/DiagnosticReports -name '*panic*' -mmin -"$MIN" 2>/dev/null | head -3
} | tee "$OUT"
echo "saved: $OUT"
