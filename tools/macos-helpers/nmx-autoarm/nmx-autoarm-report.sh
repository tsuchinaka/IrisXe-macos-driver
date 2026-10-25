#!/bin/bash
# nmx-autoarm-report.sh - run after any auto-arm test boot (also after a freeze + forced power-off):
#   sudo ./nmx-autoarm-report.sh
# Saves everything to /Users/Shared/nmx-autoarm-report.txt - send me that file.
[ "$(id -u)" = 0 ] || { echo "run with: sudo ./nmx-autoarm-report.sh"; exit 1; }
R=/Users/Shared/nmx-autoarm-report.txt
{
echo "=== $(date) autoarm report"
echo "boot-args: $(sysctl -n kern.bootargs)"
echo "--- NVRAM breadcrumbs (survive a forced power-off)"
echo "nmx-autoarm-step:       $(nvram nmx-autoarm-step 2>/dev/null | cut -f2- || true)"
echo "nullmoth-last-step:     $(nvram nullmoth-last-step 2>/dev/null | cut -f2- || true)"
echo "nullmoth-gpu-attaching: $(nvram nullmoth-gpu-attaching >/dev/null 2>&1 && echo 'SET (driver parked this boot)' || echo 'not set')"
echo "--- helper installed?"
ls -l /Library/LaunchDaemons/com.nullmoth.autoarm.plist /usr/local/libexec/nmx-autoarm.sh 2>&1
echo "--- arm state now"
sysctl debug.nvaccelfb debug.nvaccel_iop debug.nvrmfb_agdc debug.nvaccel_heads_piped debug.nvaccel_iop_ok 2>&1
echo "--- NMXGPUFB readiness properties (build 103+)"
ioreg -r -c NMXGPUFB -d 1 -l 2>/dev/null | grep -E '"NMXFB' || echo "(none - NMXGPU older than build 103, or no NMXGPUFB)"
echo "--- WindowServer / login"
echo "WindowServer pid $(pgrep -x WindowServer | head -1), up $(ps -o etime= -p "$(pgrep -x WindowServer | head -1)" 2>/dev/null)"
echo "console user: $(stat -f %Su /dev/console)"
echo "--- kernel panics (newest 3; a panic during the freeze is saved here on the next boot)"
ls -t /Library/Logs/DiagnosticReports/*anic* /Library/Logs/DiagnosticReports/Retired/*anic* 2>/dev/null | head -3
P=$(ls -t /Library/Logs/DiagnosticReports/*anic* 2>/dev/null | head -1)
[ -n "$P" ] && { echo "--- newest panic: $P"; head -c 6000 "$P"; echo; }
echo "--- /var/log/nmx-autoarm.log (last 60 lines)"
tail -60 /var/log/nmx-autoarm.log 2>&1
echo "--- kernel log, this boot, NullMoth / NMXAccel (last 60 lines)"
log show --last boot --style compact --predicate 'eventMessage CONTAINS "NullMoth" OR sender == "NMXAccel" OR sender == "NVRMAGDC"' 2>/dev/null \
  | grep -v -e "NVSurface::" -e "getDirtyBuffer" -e "DISP\[" -e "getStampMemory" -e "createUserGPUTask" -e "VBL rate" | tail -60
} > "$R" 2>&1
chmod 644 "$R"
echo "saved: $R"
grep -E "nmx-autoarm-step|nullmoth-last-step|newest panic" "$R"
