#!/bin/bash
# nmx-bright.sh - why Control Center's brightness slider shows full after a restart although the panel keeps the saved
# level. Shows how macOS classifies the display (built-in or not), what each brightness layer holds, and the
# brightness messages of this boot. Read-only.    ./nmx-bright.sh  (no sudo needed)   Report: /Users/Shared/nmx-bright.txt
O=/Users/Shared/nmx-bright.txt
{ echo "=== $(date)"
  echo "--- display classes (AppleBacklightDisplay = macOS treats it as a laptop panel)"
  ioreg -lw0 -r -c IODisplay 2>/dev/null | grep -E '^\+-o|IOClass|IODisplayParameters|DisplayProductID|DisplayVendorID|IODisplayConnectFlags|AppleDisplayType|IOBacklight|built' | head -30
  echo "--- framebuffer connection flags"
  ioreg -lw0 -r -c NMXGPUFB 2>/dev/null | grep -iE 'connect|builtin|built-in|backlight|IOFBDependent|AAPL,' | head -20
  echo "--- system_profiler"
  system_profiler SPDisplaysDataType 2>/dev/null | head -40
  echo "--- CoreBrightness / DisplayServices messages (this boot)"
  log show --last boot --style compact --predicate 'process == "corebrightnessd" OR (process == "WindowServer" AND eventMessage CONTAINS[c] "brightness") OR (process == "ControlCenter" AND eventMessage CONTAINS[c] "brightness")' 2>/dev/null | grep -iE 'brightness|display|builtin|internal|backlight' | head -60
  echo "--- CoreBrightness prefs"
  defaults read com.apple.CoreBrightness 2>&1 | head -40
  sudo -n defaults read /var/root/Library/Preferences/com.apple.CoreBrightness.plist 2>&1 | head -40
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
