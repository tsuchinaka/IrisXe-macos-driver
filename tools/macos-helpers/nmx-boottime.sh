#!/bin/bash
# nmx-boottime.sh - when did each part of the boot happen? (step 1 of "accelerated before the lock screen")
#
#   sudo ./nmx-boottime.sh            -> prints the timeline and writes ~/Desktop/nmx-boottime.txt
#
# Changes nothing. Reads this boot's log and prints, in seconds since the kernel started, when the GPU driver loaded,
# found its firmware, lit the screen, when NMXAccel loaded, when WindowServer and the login window started, when
# WindowServer opened the screen, when acceleration was armed and when WindowServer was restarted.
# Run it after a restart, once the desktop (or the lock screen) is up for a minute.
OUT="$HOME/Desktop/nmx-boottime.txt"
[ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/nmx-boottime.txt"
BOOT=$(sysctl -n kern.boottime | sed -E 's/^[^0-9]*sec = ([0-9]+).*/\1/')
START=$(date -r "$BOOT" '+%Y-%m-%d %H:%M:%S')
RAW=$(mktemp /tmp/nmx-boottime.XXXXXX)
echo "reading the log since $START (takes a little while)..."
log show --start "$START" --style syslog --info \
  --predicate '(process == "kernel" AND (eventMessage CONTAINS "NullMoth" OR eventMessage CONTAINS "NMXAccel" OR eventMessage CONTAINS "IOFramebuffer" OR eventMessage CONTAINS "BSD root")) OR process == "WindowServer" OR process == "loginwindow" OR process == "kernelmanagerd"' \
  > "$RAW" 2>/dev/null

# seconds since boot of a syslog-style line ("2026-10-24 16:30:01.123456+0530 host proc[pid]: ...")
secs() {
    local ts frac
    ts=$(cut -c1-19 <<<"$1"); frac=$(cut -c20-23 <<<"$1")
    local e; e=$(date -j -f '%Y-%m-%d %H:%M:%S' "$ts" +%s 2>/dev/null) || { echo "?"; return; }
    awk -v e="$e" -v f="0$frac" -v b="$BOOT" 'BEGIN{printf "%6.1f", e + f - b}'
}
# first line matching an extended regex
ev() {
    local label=$1 re=$2 not=${3:-NO-EXCLUDE-PATTERN} line
    line=$(grep -E "$re" "$RAW" | grep -v -E "$not" | head -1)
    if [ -n "$line" ]; then printf "%s s  %s\n" "$(secs "$line")" "$label"; else printf "     -    %s (not in the log)\n" "$label"; fi
}
{
echo "=== boot timeline $(date) - kernel started $START"
echo "    (seconds since the kernel started; '-' = did not happen / not logged this boot)"
ev "BSD root mounted"                                  'BSD root'
ev "kernelmanagerd running (loads /Library/Extensions)" 'kernelmanagerd\['
ev "NMXGPU loaded (holds the screen slot)"             'NullMoth GPU: (holding|boot guard)'
ev "NMXGPU attach started (firmware folder found)"     'NullMoth step: [0-9]+ attach: start'
ev "screen: framebuffer opened by i915"                'NullMoth step: [0-9]+ publish: framebuffer open'
ev "screen: firmware picture kept (b164)"              'still show the firmware screen'
ev "screen: NMXGPUFB registered"                       'NullMoth step: [0-9]+ publish: NMXGPUFB registered'
ev "NMXAccel loaded"                                   'NMXAccel: '
ev "WindowServer started (first)"                      'WindowServer\['
ev "loginwindow started"                               'loginwindow\['
ev "WindowServer opened the screen"                    'NullMoth step: [0-9]+ fb: opened'
ev "screen handed over to our buffer (b164)"           'boot screen hand-over: '
ev "acceleration armed (nvaccelfb accepted)"           'NMXAccel: sysctl debug.nvaccelfb=[1-4]: scanout-backed'
echo "    hold / hand-over lines:"; grep -E 'NullMoth (display|fb).*(hand-over|hold)' "$RAW" | grep -v 'DPMS\|noted, applied' | head -6 | while IFS= read -r l; do printf "  %s s  %s\n" "$(secs "$l")" "$(sed 's/.*NullMoth //' <<<"$l" | cut -c1-150)"; done
echo "    hand-over reason: $(grep -m1 -o 'boot screen hand-over: .* - committing' "$RAW" | sed 's/^boot screen hand-over: //; s/ - committing$//')"
echo
echo "--- WindowServer processes this boot (a second one = the restart after the arm)"
awk '{for (i = 3; i <= NF; i++) if ($i ~ /^WindowServer\[[0-9]+\]:?$/) { p = $i; sub(/:$/, "", p); if (!(p in seen)) { seen[p] = 1; print p, $1, $2 }; break }}' "$RAW" |
  while read -r p d t; do printf "%s s  %s\n" "$(secs "$d $t")" "$p"; done | sort -n
echo
echo "--- autoarm (this boot)"
awk -v b="$(date -r "$BOOT" '+%a %b %e')" 'index($0,"=== ")==1{keep=0} index($0,"=== ")==1 && index($0,b){keep=1} keep' /var/log/nmx-autoarm.log 2>/dev/null | tail -30
echo
echo "--- NullMoth / NMXAccel lines (first 60)"
grep -E 'NullMoth (GPU|step|display)|NMXAccel: ' "$RAW" | head -60 | cut -c1-220
} | tee "$OUT"
rm -f "$RAW"
echo
echo "saved: $OUT"
