#!/bin/bash
# nmx-blackbox.sh - armed only: the lock screen clock is missing, its date text is cut off, the unlock spinner is a black
# square (10-21 photo). Same with nmx-ws-arm.sh and auto-arm. This splits the question in two:
#   - screenshots taken WHILE the lock screen is up: macOS draws them from WindowServer's own picture. Correct
#     clock in the screenshot but black on the panel -> my copy to the panel is wrong (kernel). Black in the screenshot
#     too -> WindowServer itself drew it black (its Metal/CoreAnimation path).
#   - which processes talk to the GPU during the lock (dtrace), and every Metal plugin message + WindowServer error
# Read-only (dtrace, SIP off). Run: sudo ./nmx-blackbox.sh   then lock with Ctrl+Cmd+Q as it says.
# Report: /Users/Shared/nmx-blackbox.txt  + screenshots /Users/Shared/nmx-bb-*.png
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
[ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ] || { echo "STOP: not armed - this only happens armed"; exit 1; }
R=/Users/Shared/nmx-blackbox.txt; S=/Users/Shared
rm -f $S/nmx-bb-*.png
ws=$(pgrep -x WindowServer | head -1)
START=$(date '+%Y-%m-%d %H:%M:%S')
echo "In 5 s: LOCK THE SCREEN with Ctrl+Cmd+Q. Stay on the lock screen ~40 s (don't touch anything)."
echo "When the screen wakes/you see the lock screen for a while, type your password and unlock. Then wait for 'done'."
sleep 5
echo "LOCK NOW (Ctrl+Cmd+Q)"
dtrace -q -Z -o /var/tmp/nmx-bb-dtrace.txt -n '
fbt:com.nullmoth.NMXAccel::entry { @k[execname, probefunc] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*newUserClient*:entry { @uc[execname] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2::return /arg1 == 0xe00002c7 || arg1 == 0xe00002bc || arg1 == 0xe00002c2/ { @bad[execname, probefunc, arg1] = count(); }
tick-55s {
  printf("=== GPU calls by process during the lock (process, kernel function, count; top 60)\n"); trunc(@k, 60); printa("%-22s %-60s %@d\n", @k);
  printf("\n=== new GPU connections by process\n"); printa("%-22s %@d\n", @uc);
  printf("\n=== GPU calls that FAILED (process, function, error)\n"); printa("%-22s %-50s 0x%x %@d\n", @bad);
  exit(0); }' &
DT=$!
sleep 12
for i in 1 2 3; do
  screencapture -x "$S/nmx-bb-lock-$i.png" 2>/dev/null || echo "screenshot $i failed" >> /var/tmp/nmx-bb-notes.txt
  ps -axo pid,comm | grep -iE "lock|login|wallpaper|ScreenSaver|Clock|Notification" | grep -v grep >> /var/tmp/nmx-bb-ps.txt
  sleep 8
done
echo "you can unlock now (type your password)"
wait $DT
{
echo "=== $(date) black box report, WindowServer $ws (now $(pgrep -x WindowServer | head -1)), boot-args: $(sysctl -n kern.bootargs)"
echo "screenshots: $(ls -l $S/nmx-bb-*.png 2>/dev/null | awk '{print $5, $9}' | tr '\n' ' ')"
cat /var/tmp/nmx-bb-notes.txt 2>/dev/null
echo "--- lock-screen processes seen"; sort -u /var/tmp/nmx-bb-ps.txt 2>/dev/null
echo "--- allow-list"; cat /Library/GPUBundles/nvmtl-allow.txt 2>/dev/null
echo; cat /var/tmp/nmx-bb-dtrace.txt
echo; echo "--- Metal plugin messages since the start, by process (count, message)"
log show --start "$START" --style compact --predicate 'eventMessage CONTAINS "NVMTL"' 2>/dev/null \
  | sed -E 's/^[0-9-]+ [0-9:.]+ +[A-Za-z]+ +//; s/\[[0-9]+:[0-9a-f]+\]//; s/0x[0-9a-f]+/0x_/g; s/[0-9]{3,}/N/g' | sort | uniq -c | sort -rn | head -150
echo; echo "--- WindowServer errors / faults since the start"
log show --start "$START" --style compact --predicate 'process == "WindowServer" AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "fail" OR eventMessage CONTAINS[c] "metal")' 2>/dev/null \
  | sed -E 's/^[0-9-]+ [0-9:.]+ +//; s/0x[0-9a-f]+/0x_/g' | sort | uniq -c | sort -rn | head -60
echo; echo "--- lock screen / loginwindow errors since the start"
log show --start "$START" --style compact --predicate '(process CONTAINS[c] "lock" OR process == "loginwindow") AND (messageType == error OR messageType == fault OR eventMessage CONTAINS[c] "metal" OR eventMessage CONTAINS[c] "render")' 2>/dev/null \
  | sed -E 's/^[0-9-]+ [0-9:.]+ +//; s/0x[0-9a-f]+/0x_/g' | sort | uniq -c | sort -rn | head -60
echo; echo "--- kernel: NMXAccel frame / copy-forward lines since the start (last 30)"
log show --start "$START" --style compact --predicate 'sender == "NMXAccel"' 2>/dev/null | grep -E "frames:|new front|never received|copyfwd" | awk '!seen[$0]++' | tail -30
} > "$R" 2>&1
rm -f /var/tmp/nmx-bb-*.txt
chmod 644 "$R" $S/nmx-bb-*.png 2>/dev/null
echo "done. Send me: $R and the pictures $S/nmx-bb-lock-1.png 2 3"
