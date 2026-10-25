#!/bin/bash
# nmx-display.sh [minutes] - what the driver did with the displays (external monitor / TV test). Changes nothing.
#
#   sudo ./nmx-display.sh 15     -> the last 15 minutes (default 30), writes ~/Desktop/nmx-display.txt
#
# Shows: each output the driver sees (eDP-1 = the laptop panel, HDMI-A-1 / DP-1 ... = external), its mode, the picture
# size it shows, hot-plug events, refused mode sets (the external is then left dark and the panel keeps working), and
# what macOS itself reports about displays.
MIN=${1:-30}
OUT="$HOME/Desktop/nmx-display.txt"
[ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/nmx-display.txt"
{
echo "=== displays $(date), last $MIN min - boot-args: $(sysctl -n kern.bootargs)"
echo
echo "--- driver: outputs, hot-plug, mode sets"
log show --last "${MIN}m" --style compact --predicate 'sender == "NMXGPU"' 2>/dev/null \
  | grep -E 'NullMoth display: (output|no output|hotplug|.*refused|.*mode set|boot screen hand-over|desktop now|DPMS)|connector|HDMI|DP-|hotplug|HPD|EDID|link train' \
  | grep -v 'NullMoth step' | tail -80 | cut -c1-240
echo
echo "--- driver: outputs this boot (first lines)"
log show --last boot --style compact --predicate 'sender == "NMXGPU" AND eventMessage CONTAINS "NullMoth display: output"' 2>/dev/null | head -10 | cut -c1-240
echo
echo "--- macOS"
system_profiler SPDisplaysDataType 2>/dev/null | sed -n '/Displays:/,$p'
} | tee "$OUT"
echo "saved: $OUT"
