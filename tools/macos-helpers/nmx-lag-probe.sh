#!/bin/bash
# nmx-lag-probe.sh (v2) - where does the remaining lag go? While you type or scroll for 10 s it records:
#   - which processes use the CPU (top, every second) - finds the real culprit even when it is not the app
#   - what WindowServer and the app do (Apple's `sample`), summed up as CPU compositing / GPU / waiting / ...
#   sudo ./nmx-lag-probe.sh "System Settings"       then scroll fast / type for the 10 seconds it records
# Report: /Users/Shared/nmx-lag-<app>.txt   raw samples: /Users/Shared/nmx-lag-raw-<app>/
# v1 (10-20) sampled unrelated widget processes and both runs wrote the same raw folder; v2 fixes both.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
j="$*"; j=${j//“/}; j=${j//”/}; APP=${j:-TextEdit}
SEC=10; safe=$(tr ' /' '__' <<<"$APP"); R=/Users/Shared/nmx-lag-$safe.txt; RAW=/Users/Shared/nmx-lag-raw-$safe
rm -rf "$RAW"; mkdir -p "$RAW"
ws=$(pgrep -x WindowServer | head -1); ap=$(pgrep -x "$APP" | head -1)
[ -n "$ws" ] || { echo "no WindowServer?"; exit 1; }
[ -n "$ap" ] || { echo "\"$APP\" is not running - open it first, then run this again"; exit 1; }
echo "Recording $SEC s in 2 s - click into $APP and keep scrolling fast / typing until it says done"
sleep 2; echo "NOW"
top -l $((SEC + 1)) -s 1 -o cpu -n 15 -stats pid,command,cpu > "$RAW/top.txt" 2>/dev/null &
sample "$ws" $SEC -mayDie -file "$RAW/ws.txt" >/dev/null 2>&1 &
sample "$ap" $SEC -mayDie -file "$RAW/app.txt" >/dev/null 2>&1 &
wait
echo "done - writing the report"
census() {
    f=$1; [ -f "$f" ] || { echo "  (no sample)"; return; }
    for k in "CPU compositor (CA software):CompositorSW|SWContext|layer_blit|CA::SW" \
             "GPU compositor (Metal):CompositorMetal|CA::OGL|CA::Render::Metal" \
             "our plugin / Vulkan:NVMTL|nvmtl_|vk[A-Z]|anv_" \
             "display surface lock (kernel):IOAccelWriteLockSurface|IOAccelSetSurfaceFramebufferShape" \
             "CPU pixel copy/convert:argb32_|vImage|ripc_|RIPLayer|CGBlt|ripd_|CGSConvert" \
             "text / spell / predictions:NSSpellChecker|NSTextCheck|TextInputUI|Candidate|CTFont|TLine" \
             "icons / image decode:IconServices|ISIcon|ISImage|CGImageSource|ImageIO|AppleJPEG" ; do
        name=${k%%:*}; pat=${k#*:}
        printf "  %-36s %6d stack lines\n" "$name" "$(grep -cE "$pat" "$f")"
    done
}
# busy time of the main thread: all its samples minus the ones sitting in the event-loop wait (1 sample = 1 ms)
mainbusy() {
    f=$1; [ -f "$f" ] || return
    awk '/^    [0-9]+ Thread_.*(Main Thread|ws_main_thread)/{tot=$1; m=1; next}
         m && /^    [0-9]+ Thread_/{exit}
         m && (/__CFRunLoopServiceMachPort/ || /CGXRunOneServicesPass .*\+ 734 /) { for (i=1;i<=NF;i++) if ($i ~ /^[0-9]+$/) { idle+=$i; break } }
         END{ if (tot) printf "  main thread: busy ~%d ms of %d ms (%d%%)\n", tot-idle, tot, (tot-idle)*100/tot }' "$f"
}
topavg() {
    awk '/^PID/{blk++; next} blk>1 && NF>=3 { cpu=$NF; pid=$1; $1=""; $NF=""; name=$0; gsub(/^ +| +$/,"",name);
         if (cpu ~ /^[0-9.]+$/) { s[pid]+=cpu; n[pid]=name } }
         END{ for (p in s) printf "  %6.1f%%  %s (%s)\n", s[p]/(blk-1), n[p], p }' "$RAW/top.txt" | sort -rn | head -12
}
top_of_stack() { f=$1; [ -f "$f" ] || return; awk '/Sort by top of stack/{p=1;next} p&&/Binary Images/{exit} p&&NF{print; n++} n>=20{exit}' "$f" | sed 's/^/  /'; }
{
echo "=== $(date) lag probe v2: $APP ($SEC s), WindowServer $ws, app $ap"
echo "GPU list:"; grep -v '^#' /Library/GPUBundles/nvmtl-allow.txt | sed 's/^/  /'
echo; echo "### busiest processes during the recording (average CPU, 100% = one core)"; topavg
echo; echo "### WindowServer"; mainbusy "$RAW/ws.txt"; census "$RAW/ws.txt"
echo; echo "### $APP"; mainbusy "$RAW/app.txt"; census "$RAW/app.txt"
echo; echo "### WindowServer - top of stack"; top_of_stack "$RAW/ws.txt"
echo; echo "### $APP - top of stack"; top_of_stack "$RAW/app.txt"
} > "$R" 2>&1
chmod -R a+rX "$R" "$RAW"
echo "Report for me: $R   (raw: $RAW)"
sed -n '/busiest processes/,/^$/p' "$R"
