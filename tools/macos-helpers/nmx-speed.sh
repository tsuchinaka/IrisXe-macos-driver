#!/bin/bash
# nmx-speed.sh [seconds] - where the CPU time goes while the desktop runs (speed / heat, b190). Changes nothing.
#
#   sudo ./nmx-speed.sh 60      -> writes ~/Desktop/nmx-speed-<date>-<time>.txt (a new file every run)
#
# For the given time (default 60 s) it measures how many CPU seconds the graphics-related processes use (WindowServer,
# logd, kernel_task, iconservicesagent, Dock, Finder, System Settings) and how many log lines our drivers write. Run it
# twice: once with the desktop idle, once while you open Applications / System Settings / scroll a Safari page, and
# compare. Before / after a new build shows whether that build made the desktop cheaper.
SECS=${1:-60}
STAMP=$(date +%m%d-%H%M)
OUT="$HOME/Desktop/nmx-speed-$STAMP.txt"
[ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/nmx-speed-$STAMP.txt"
NAMES="WindowServer logd kernel_task iconservicesagent Dock Finder System?Settings Safari mds_stores"
cpu_of() {  # total CPU seconds of all processes whose command name matches $1 (? = space)
    local pat=${1//\?/ }
    ps -axo time=,comm= | awk -v p="$pat" '{
        t=$1; $1=""; sub(/^ /,""); n=$0; sub(/.*\//,"",n)
        if (n != p) next
        k=split(t,a,":"); s=0
        if (k==3) s=a[1]*3600+a[2]*60+a[3]; else if (k==2) s=a[1]*60+a[2]; else s=a[1]
        tot+=s } END { printf "%.2f", tot+0 }'
}
# macOS bash 3.2 has no associative arrays: one variable per name (B_<index>)
i=0; for n in $NAMES; do eval "B_$i=$(cpu_of "$n")"; i=$((i + 1)); done
t0=$(date "+%Y-%m-%d %H:%M:%S")
sleep "$SECS"
{
echo "=== nmx-speed $(date), ${SECS} s, macOS $(sw_vers -productVersion), uptime $(uptime | sed 's/.*up //; s/,.*//')"
echo
echo "--- CPU seconds used in these ${SECS} s (100% of one core = ${SECS} s)"
i=0; for n in $NAMES; do
    eval "b=\$B_$i"; i=$((i + 1))
    a=$(cpu_of "$n"); d=$(awk -v a="$a" -v b="$b" 'BEGIN{ printf "%.2f", a - b }')
    p=$(awk -v d="$d" -v s="$SECS" 'BEGIN{ printf "%.0f", d * 100 / s }')
    printf "  %-20s %7s s  (%s%% of a core)\n" "${n//\?/ }" "$d" "$p"
done
echo
echo "--- log lines written in these ${SECS} s"
L=$(log show --start "$t0" --style compact 2>/dev/null | grep -v '^Timestamp')
echo "  all processes:            $(printf '%s\n' "$L" | grep -c .)"
echo "  NMXGPU / NMXAccel (kernel): $(printf '%s\n' "$L" | grep -cE '\((NMXGPU|NMXAccel)\)')"
echo "  NMXGPU per-commit lines:  $(printf '%s\n' "$L" | grep -c 'NullMoth commit:')"
echo "  Metal plugin (NVMTL):     $(printf '%s\n' "$L" | grep -cE 'NVMTL|NullMoth: ')"
echo "  plugin build: $(grep -aq 'nvmtl-off.log' /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver 2>/dev/null && echo QUIET || echo VERBOSE)"
echo
echo "--- busiest processes right now"
top -l 2 -o cpu -n 6 -stats pid,command,cpu 2>/dev/null | tail -7
} | tee "$OUT"
echo "saved: $OUT"
