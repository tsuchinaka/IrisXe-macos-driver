#!/bin/bash
# nmx-icontest.sh on|collect|off - b191 icon test (docs/SPEEDUP.md S3), Tahoe.
#
#   sudo ./nmx-icontest.sh on        lets iconservicesagent use the GPU again, clears the icon cache, turns capture on.
#                                    Then open System Settings and Applications (the yellow squares come back).
#   sudo ./nmx-icontest.sh collect   packs the saved passes + log into ~/Desktop/nmx-icontest.tar.gz
#   sudo ./nmx-icontest.sh off       back to normal: iconservicesagent off the GPU, icon cache cleared, capture off.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
A=/Library/GPUBundles/nvmtl-allow.txt; F=/Library/GPUBundles/nvmtl-icontest
U=${SUDO_USER:-$(stat -f %Su /dev/console)}; D="/Users/$U/Desktop"
clear_icons() { rm -rf /Library/Caches/com.apple.iconservices.store; killall iconservicesagent 2>/dev/null; killall Dock 2>/dev/null; }
case "${1:-}" in
on)
    touch "$F"
    grep -v '^ *- *iconservicesagent *$' "$A" > "$A.tmp" && cat "$A.tmp" > "$A"; rm -f "$A.tmp"
    find /private/var/folders -type d -name nvmtl-icontest -path '*iconservicesagent*' -exec rm -rf {} + 2>/dev/null
    clear_icons
    echo "icon test ON - now open System Settings and Applications, wait ~20 s, then: sudo $0 collect" ;;
collect)
    T=$(mktemp -d)
    find /private/var/folders -type d -name nvmtl-icontest -path '*iconservicesagent*' 2>/dev/null | while read -r d; do cp -R "$d" "$T/"; done
    log show --last 30m --predicate 'eventMessage CONTAINS "ICONTEST" OR (process == "iconservicesagent" AND eventMessage CONTAINS "NullMoth")' > "$T/icontest-log.txt" 2>&1
    tar -czf "$D/nmx-icontest.tar.gz" -C "$T" . && chown "$U" "$D/nmx-icontest.tar.gz"; rm -rf "$T"
    echo "saved: $D/nmx-icontest.tar.gz ($(du -h "$D/nmx-icontest.tar.gz" | cut -f1))" ;;
off)
    rm -f "$F"
    if grep -aq 'b211; refused before' /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver 2>/dev/null && [ ! -e /Library/GPUBundles/nvmtl-iconcpu ]; then
        clear_icons; echo "icon test OFF - icons stay on the GPU (plugin b211 or later)"
    else
        grep -q '^ *- *iconservicesagent *$' "$A" || { { echo '- iconservicesagent'; cat "$A"; } > "$A.tmp" && cat "$A.tmp" > "$A"; rm -f "$A.tmp"; }
        clear_icons
        echo "icon test OFF - iconservicesagent is off the GPU again, icons are redrawn on the CPU"
    fi ;;
*) sed -n 2,8p "$0"; exit 1 ;;
esac
