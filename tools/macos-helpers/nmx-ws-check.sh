#!/bin/bash
# nmx-ws-check.sh - after nmx-ws-arm.sh and logging back in: is the desktop on the GPU? Writes ~/nmx-ws-report.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo ./nmx-ws-check.sh"; exit 1; }
U=$(eval echo "~${SUDO_USER:-$USER}"); R="/Users/Shared/nmx-ws-report.txt"
{
echo "=== $(date)"; sysctl kern.bootargs
WS=$(pgrep -x WindowServer | head -1); echo "WindowServer pid $WS"
sysctl debug.nvaccelfb debug.nvaccel_iop debug.nvaccel_iop_ok debug.nvaccel_iop_fail debug.nvaccel_iop_empty debug.nvaccel_iop_blank \
       debug.nvaccel_iop_src_surf debug.nvaccel_heads_published debug.nvaccel_heads_piped debug.nvaccel_vm_refused debug.nvaccel_res_new debug.nvrmfb_agdc debug.nvrmfb_agdc_cmds debug.nvrmfb_agdc_refused debug.nvrmfb_agdc_last_refused debug.nvrmfb_agdc_fbmap debug.nvrmfb_agdc_k5 debug.nvrmfb_agdc_k5_link debug.nvrmfb_agdc_k5_why debug.nvaccel_iop_ok 2>&1
echo "--- AGDC + display pipe"; log show --last 15m --style compact --predicate 'eventMessage CONTAINS "agdc" OR eventMessage CONTAINS "display pipe" OR eventMessage CONTAINS "IOPresentment" OR eventMessage CONTAINS "NVRMTiming"' 2>/dev/null | grep -v "DISP\\[" | tail -60
echo "--- metal log"; tail -40 "$(find /var/folders -name nvmtl-ws.log 2>/dev/null | head -1)" 2>&1
echo "--- plugin log (WindowServer)"; grep -a "pid $WS " /tmp/nvmtl.log 2>/dev/null | head -60
echo "--- plugin log (any WindowServer line)"; grep -a "WindowServer" /tmp/nvmtl.log 2>/dev/null | tail -20
echo "--- kernel (NMXAccel / display)"; log show --last 15m --style compact --predicate 'eventMessage CONTAINS "NMXAccel" OR eventMessage CONTAINS "NullMoth display"' 2>/dev/null | tail -80
echo "--- WindowServer messages"; log show --last 15m --style compact --predicate "process == \"WindowServer\" AND (eventMessage CONTAINS[c] \"metal\" OR eventMessage CONTAINS[c] \"accel\" OR eventMessage CONTAINS[c] \"error\")" 2>/dev/null | tail -40
echo "--- arm log"; tail -30 /var/log/nmx-ws.log 2>/dev/null
# b106 (lag probe 10-20): WindowServer composites the SCREEN with CompositorSW (CPU) through the legacy display surface
# (CDDisplay::render_update -> CGXAccessDisplayDeviceSurface -> IOAccelWriteLockSurface), Metal only for captures.
# Which display path is live: display-pipe transactions (iop_ok rising = IOPresentment, Metal compositor) or the legacy
# surface swap (submitSwapCopy, iop_ok flat)? Measured over 5 s while the screen changes.
echo "--- display path over 5 s (move a window or type during this)"
a=$(sysctl -n debug.nvaccel_iop_ok 2>/dev/null); b=$(sysctl -n debug.nvaccel_iop_empty 2>/dev/null); f=$(sysctl -n debug.nvaccel_iop_fail 2>/dev/null)
sleep 5
echo "iop_ok +$(( $(sysctl -n debug.nvaccel_iop_ok 2>/dev/null) - a ))  iop_empty +$(( $(sysctl -n debug.nvaccel_iop_empty 2>/dev/null) - b ))  iop_fail +$(( $(sysctl -n debug.nvaccel_iop_fail 2>/dev/null) - f ))  (0 = WindowServer is NOT using display-pipe transactions)"
echo "--- WindowServer / CoreDisplay compositor choice (this boot)"
log show --last boot --style compact --predicate 'process == "WindowServer" AND (eventMessage CONTAINS[c] "compositor" OR eventMessage CONTAINS[c] "software" OR eventMessage CONTAINS[c] "IOPresentment" OR eventMessage CONTAINS[c] "display pipe" OR eventMessage CONTAINS[c] "MetalDevice" OR eventMessage CONTAINS[c] "fallback")' 2>/dev/null | tail -60
echo "--- kernel swap path lines"; log show --last boot --style compact --predicate 'eventMessage CONTAINS "present path" OR eventMessage CONTAINS "swap" OR eventMessage CONTAINS "iop:"' 2>/dev/null | grep -v "DISP\\[" | tail -40
echo "--- windowserver defaults"; defaults read /Library/Preferences/com.apple.windowserver.plist 2>&1 | head -40
} > "$R" 2>&1
chown "${SUDO_USER:-$USER}" "$R"; echo "wrote $R - send it to me"
