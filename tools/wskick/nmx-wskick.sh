#!/bin/bash
# nmx-wskick.sh - runs once at boot (LaunchDaemon com.nullmoth.wskick).
# NMXGPU attaches from a thread after the disks mount; if WindowServer starts first it logs
# "CGXMappedDisplayStart: can't open any framebuffer" and never looks again -> black screen.
# Only in exactly that case (that message, from the WindowServer running now) is WindowServer restarted, once.
# v2 (10-17): v1 decided from system_profiler's "Resolution:" line, which NMXGPUFB never shows, so it restarted a
# WindowServer that was working - and the restarted one did not get the screen back.
LOG=/var/log/nmx-wskick.log
exec >>"$LOG" 2>&1
echo "=== $(date) boot (v2)"
has_fb() { ioreg -r -c NMXGPUFB -d 1 2>/dev/null | grep -q NMXGPUFB; }
for i in $(seq 1 180); do has_fb && break; sleep 1; done
has_fb || { echo "no NMXGPUFB after 180 s (not the NMXGPU config?) - nothing to do"; exit 0; }
echo "NMXGPUFB present after ${i}s"
for i in $(seq 1 90); do pid=$(pgrep -x WindowServer | head -1); [ -n "$pid" ] && break; sleep 1; done
[ -n "$pid" ] || { echo "no WindowServer after 90 s - nothing to do"; exit 0; }
sleep 5
if log show --last 5m --style compact --predicate "processID == $pid AND eventMessage CONTAINS \"can't open any framebuffer\"" 2>/dev/null \
     | grep -q "can't open any framebuffer"; then
    echo "WindowServer $pid started before the screen existed - restarting it once"
    kill -9 "$pid"
else
    echo "WindowServer $pid opened the screen - ok, nothing done"
fi
exit 0
