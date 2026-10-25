#!/bin/bash
# nmx-boot-report.sh - what NMXGPU did in every boot of the last N hours (default 12), including boots that hung.
# Run: sudo ./nmx-boot-report.sh   (or: sudo ./nmx-boot-report.sh 24)   -> writes ~/nmx-boot-report.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo ./nmx-boot-report.sh"; exit 1; }
H="${1:-12}"; U=$(eval echo "~${SUDO_USER:-$USER}"); R="$U/nmx-boot-report.txt"
{
echo "=== $(date)   boot-args: $(sysctl -n kern.bootargs)"
echo "this boot started: $(sysctl -n kern.boottime)"
echo "--- boot guard in NVRAM (set = the last attach never finished):"; nvram -p 2>/dev/null | grep -i "nullmoth\|nmx" || echo "(none)"
echo "--- shutdown causes (last 10):"; log show --last "${H}h" --style compact --predicate 'eventMessage CONTAINS "Previous shutdown cause"' 2>/dev/null | tail -10
echo "--- NullMoth GPU steps, per boot:"
log show --last "${H}h" --style compact --predicate 'eventMessage CONTAINS "NullMoth GPU" OR eventMessage CONTAINS "nmx step" OR eventMessage CONTAINS "NullMoth display" OR eventMessage CONTAINS "NullMoth step" OR eventMessage CONTAINS "boot guard" OR eventMessage CONTAINS "=== system boot" OR (process == "kernel" AND eventMessage BEGINSWITH "Darwin Kernel Version")' 2>/dev/null \
  | grep -v "DISP\[" | grep -v "backlight: " | cut -c1-260
echo "--- wskick log:"; tail -20 /var/log/nmx-wskick.log 2>/dev/null
} > "$R" 2>&1
chown "${SUDO_USER:-$USER}" "$R"; echo "wrote $R ($(wc -l < "$R") lines) - send it to me"
