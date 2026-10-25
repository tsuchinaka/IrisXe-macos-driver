#!/bin/bash
# nmx-band.sh - colour banding: which colour formats the Metal plugin refused (each refusal = macOS falls back to a
# lower-precision path), what the panel and the display pipe run at, and the WindowServer surface format. Read-only.
#   sudo ./nmx-band.sh       Report: /Users/Shared/nmx-band.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-band.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "--- formats the plugin refused (all processes, this boot's log)"
  grep -h "NOT carried\|pixel format" /tmp/nvmtl.log 2>/dev/null | sed 's/^.*pixel format/pixel format/' | sort | uniq -c | sort -rn | head -20
  echo "--- which processes asked (last 40 refusals)"
  grep -h "NOT carried" /tmp/nvmtl.log 2>/dev/null | tail -40
  echo "--- texture/format messages mentioning 10-bit / XR / half float"
  grep -hiE "XR|10A2|16Float|RGBA16|extended|EDR|wide" /tmp/nvmtl.log 2>/dev/null | grep -viE 'allow|armed' | sort | uniq -c | sort -rn | head -20
  echo "--- panel (EDID) and display pipe"
  ioreg -lw0 -r -c AppleBacklightDisplay 2>/dev/null | grep -oE '"IODisplayEDID(Original)?" = <[0-9a-f]{0,60}' | head -2
  log show --last boot --style compact --predicate 'eventMessage CONTAINS "NullMoth readout" OR eventMessage CONTAINS[c] "bpc" OR eventMessage CONTAINS[c] "dither" OR eventMessage CONTAINS[c] "pipe_bpp"' 2>/dev/null | tail -10
  echo "--- WindowServer surfaces / display depth"
  log show --last boot --style compact --predicate 'eventMessage CONTAINS "iop: K6 plane 0"' 2>/dev/null | tail -2
  system_profiler SPDisplaysDataType 2>/dev/null | grep -iE 'depth|resolution|color'
  defaults read /Library/Preferences/com.apple.windowserver.plist 2>/dev/null | grep -iE 'depth|dither|EDR|HDR' | head
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
