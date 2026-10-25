#!/bin/bash
# nmx-app-gpu.sh - Phase 4: give ONE app the Iris Xe Metal device, test it, keep it or take it back.
#
#   sudo ./nmx-app-gpu.sh test "TextEdit"                       open TextEdit on the GPU, watch it 30 s, print a verdict
#   sudo ./nmx-app-gpu.sh test "Safari" com.apple.WebKit.GPU    an app + its helper processes (all get the GPU)
#   sudo ./nmx-app-gpu.sh remove "TextEdit"                     back to the CPU (also: remove com.apple.WebKit.GPU ...)
#   sudo ./nmx-app-gpu.sh list                                  what is on the GPU now
#   sudo ./nmx-app-gpu.sh procs "Safari"                        the process names an app runs (to find its helpers)
#
# Entries are written armed-only ("!Name"): they act only while acceleration is armed, so an unarmed boot is always
# plain macOS. Freeze guard: while a test runs, NVRAM nmx-app-test holds the names under test. If the Mac freezes and
# you force it off, the next run of this script (and the auto-arm helper) sees it and takes those names off the list.
# Report of every test: /Users/Shared/nmx-app-<name>.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
A=/Library/GPUBundles/nvmtl-allow.txt
cmd=${1:-help}; shift || true
# typed with curly quotes (“System Settings”, macOS smart quotes): the shell splits them into '“System' 'Settings”'.
# Join everything back into ONE name without the curly quotes (helpers then need straight quotes).
case "$*" in *“*|*”*|*‘*|*’*) j="$*"; j=${j//“/}; j=${j//”/}; j=${j//‘/}; j=${j//’/}; set -- "$j"
    echo "(curly quotes found - using the name: $j. Type straight quotes \" next time.)";; esac

armed() { [ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ]; }
user=$(stat -f %Su /dev/console 2>/dev/null); uid=$(id -u "$user" 2>/dev/null)
asuser() { launchctl asuser "$uid" sudo -u "$user" "$@"; }
add() { grep -qxF "!$1" "$A" || echo "!$1" >> "$A"; }
del() { grep -vxF "!$1" "$A" > "$A.tmp"; cat "$A.tmp" > "$A"; rm -f "$A.tmp"; }

# names that froze the Mac in an earlier test come off the list before anything else happens
leftover=$(nvram nmx-app-test 2>/dev/null | cut -f2-)
if [ -n "$leftover" ]; then
    echo "!! The last test did not finish (the Mac froze or was turned off): $leftover"
    IFS='|' read -r -a bad <<<"$leftover"
    for n in "${bad[@]}"; do [ -n "$n" ] && { del "$n"; echo "   removed from the GPU list: $n"; }; done
    nvram -d nmx-app-test
    echo "$(date) $leftover froze the Mac (removed)" >> /Users/Shared/nmx-app-frozen.txt
fi


case "$cmd" in
list)
    echo "GPU list ($A):"; grep -v '^#' "$A" | sed 's/^/  /'
    armed && echo "acceleration: ARMED" || echo "acceleration: not armed (only armed entries exist, so nothing is on the GPU now)"
    exit 0;;
procs)
    app=${1:?app name}
    echo "processes whose path contains \"$app\" (give the helper names to: test \"$app\" <helper> ...):"
    ps -axo comm= | grep -F "$app" | sed 's|.*/||' | sort -u | sed 's/^/  /'
    exit 0;;
clean)
    grep -v '[“”‘’]' "$A" > "$A.tmp"; n=$(( $(wc -l < "$A") - $(wc -l < "$A.tmp") )); cat "$A.tmp" > "$A"; rm -f "$A.tmp"
    echo "removed $n line(s) with curly quotes"; grep -v '^#' "$A" | sed 's/^/  /'; exit 0;;
remove)
    [ $# -gt 0 ] || { echo "usage: remove <name> [name...]"; exit 1; }
    for n in "$@"; do del "$n"; echo "removed: $n (restart the app: it is back on the CPU)"; done
    exit 0;;
test) ;;
*) sed -n 2,14p "$0"; exit 0;;
esac

app=${1:?usage: test "<App Name>" [helper process names...]}; shift
names=("$app" "$@")
armed || { echo "STOP: acceleration is not armed - run sudo ./nmx-ws-arm.sh first, log in again, then test"; exit 1; }
[ -n "$user" ] && [ "$user" != root ] || { echo "STOP: nobody is logged in"; exit 1; }
safe=$(tr ' /' '__' <<<"$app"); R=/Users/Shared/nmx-app-$safe.txt
wspid=$(pgrep -x WindowServer | head -1)

echo "1/4 quitting $app if it is open"
asuser osascript -e "quit app \"$app\"" >/dev/null 2>&1; sleep 2; pkill -x "$app" 2>/dev/null; sleep 1

echo "2/4 putting it on the GPU: ${names[*]}"
cp "$A" "$A.before-$safe"
for n in "${names[@]}"; do add "$n"; done
nvram nmx-app-test="$(IFS='|'; echo "${names[*]}")"; sync
T0=$(date "+%Y-%m-%d %H:%M:%S")

echo "3/4 opening $app - USE IT now for 30 s (type, scroll, open a window)"
asuser open -a "$app" || { echo "could not open \"$app\" (use the name shown in /Applications) - list left as it was"
    for n in "${names[@]}"; do del "$n"; done; nvram -d nmx-app-test; exit 1; }
alive=1
for i in $(seq 1 30); do sleep 1; pgrep -x "$app" >/dev/null || { alive=0; break; }; done
nvram -d nmx-app-test; sync     # no freeze: the guard comes off

echo "4/4 collecting"
pred=""; for n in "${names[@]}"; do pred="$pred${pred:+ OR }process == \"$n\""; done
L=$(log show --start "$T0" --style compact --predicate "($pred) AND eventMessage CONTAINS \"NVMTL\"" 2>/dev/null)
got=$(grep -c "allow-listed" <<<"$L"); fail=$(grep -ciE "FAILED|refusing|produced nothing|not supported|unsupported|abort" <<<"$L")
oom=$(grep -c "ALLOCATION FAILING" <<<"$L")
nodev=$(grep -c "refusing to be the Metal device" <<<"$L")
# only crash reports of the processes under test (or WindowServer) written since the test began: other files in that
# folder get their dates touched by the system (10-20: a bluetoothd report from 10-17 counted as a TextEdit crash)
newcrash=""
for n in "${names[@]}" WindowServer; do
    f=$(find "/Users/$user/Library/Logs/DiagnosticReports" /Library/Logs/DiagnosticReports -maxdepth 1 -newermt "$T0" \
          \( -name "$n-*.ips" -o -name "*_$n-*.ips" \) 2>/dev/null)
    [ -n "$f" ] && newcrash="$newcrash$f
"
done
newcrash=$(sed '/^$/d' <<<"$newcrash")
ws_now=$(pgrep -x WindowServer | head -1)
{
echo "=== $(date) app test: ${names[*]}"
echo "macOS $(sw_vers -productVersion), armed $(sysctl -n debug.nvaccelfb), GPU list:"; grep -v '^#' "$A" | sed 's/^/  /'
echo "Metal device given (allow-listed lines): $got   problem lines: $fail   out-of-memory lines: $oom"
echo "app still running after 30 s: $([ $alive = 1 ] && echo yes || echo NO)"
echo "WindowServer: $wspid -> $ws_now $([ "$wspid" = "$ws_now" ] || echo '(RESTARTED - it crashed)')"
echo "new crash reports:"; [ -n "$newcrash" ] && echo "$newcrash" | sed 's/^/  /' || echo "  none"
while IFS= read -r c; do [ -n "$c" ] && { echo "--- $c"; head -c 4000 "$c"; echo; }; done <<<"$newcrash"
echo "--- plugin log lines (last 150)"; tail -150 <<<"$L"
} > "$R" 2>&1
chmod 644 "$R"

echo
if [ "$got" != 0 ] && [ "$nodev" != 0 ]; then
    echo "RESULT: $app was allowed the GPU, but the GPU did not start inside it (\"refusing to be the Metal device\")."
    echo "        It ran on the CPU as before. Send me the report - this is a plugin fix, not an app problem."
elif [ "$got" = 0 ]; then
    echo "RESULT: $app did NOT get the GPU device (no 'allow-listed' line). It ran on the CPU as before."
    echo "        Maybe its drawing happens in a helper process - see: sudo $0 procs \"$app\""
elif [ $alive = 0 ] || [ -n "$newcrash" ] || [ "$wspid" != "$ws_now" ]; then
    echo "RESULT: FAILED - $app got the GPU but crashed (or took WindowServer down). Taking it off the list."
    for n in "${names[@]}"; do del "$n"; done
else
    echo "RESULT: $app is running on the GPU (problem lines: $fail). Check: does it look right? Is typing faster?"
    echo "        Looks wrong (black, garbage, missing parts)? -> sudo $0 remove ${names[*]}"
fi
if grep -q "in-memory bitcode reader unavailable" <<<"$L"; then
    echo "NOTE: the plugin's shader reader (air-runtime/libLLVM.dylib) is MISSING - apps cannot compile their shaders."
    echo "      Fix first: sudo ./nmx-plugin-update.sh <new bundle> <full bundle from the Mesa ANV + Metal plugin artifact>"
fi
echo "Report for me: $R"
