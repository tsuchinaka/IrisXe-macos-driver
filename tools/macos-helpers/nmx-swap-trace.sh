#!/bin/bash
# nmx-swap-trace.sh - b108 + nmxcopyfwd=1 flickers between the old lock screen and the desktop (10-20). Records, for
# 60 s after the next WindowServer restart, every swap (which surface, which buffers a1/a2), which buffer was copied to
# the panel, and every copy forward (which buffers) - with real pointers, so two surfaces/buffers can be told apart.
# It runs from a one-shot LaunchDaemon, so it keeps recording while the screen is unusable.
#   1. sudo ./nmx-swap-trace.sh          (installs it - nothing is recorded yet)
#   2. sudo ./nmx-ws-arm.sh              (arm as usual; the recording starts 10 s after WindowServer restarts)
#   3. log in, wait 1 minute, then hold the power button / restart WITHOUT arming
#   4. send /Users/Shared/nmx-swap-trace.txt
# Read-only (dtrace, SIP off). The job removes itself after one run.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
cat > /var/tmp/nmx-swap-trace-run.sh <<'RUN'
#!/bin/bash
R=/Users/Shared/nmx-swap-trace.txt
old=$(pgrep -x WindowServer | head -1)
# wait for the WindowServer restart that nmx-ws-arm.sh does (up to 10 minutes), then 10 s more
for i in $(seq 1 600); do sleep 1; n=$(pgrep -x WindowServer | head -1); [ -n "$n" ] && [ "$n" != "$old" ] && break; done
sleep 10
{
echo "=== $(date) swap trace (WindowServer $old -> $(pgrep -x WindowServer | head -1)), boot-args: $(sysctl -n kern.bootargs)"
dtrace -q -Z -n '
BEGIN { t0 = timestamp; }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry { printf("%6d swap  surf %p a1 %p a2 %p\n", (timestamp - t0) / 1000000, arg0, arg2, arg3); @surf[arg0] = count(); }
fbt:com.nullmoth.NMXAccel:*nvAccelResHasContent*:entry { self->hc = arg0; }
fbt:com.nullmoth.NMXAccel:*nvAccelResHasContent*:return /self->hc/ { printf("%6d   content %p = %d\n", (timestamp - t0) / 1000000, self->hc, arg1 & 0xff); self->hc = 0; }
fbt:com.nullmoth.NMXAccel:*nvAccelCopyToScanout*:entry { printf("%6d   PANEL <- %p\n", (timestamp - t0) / 1000000, arg0); @panel[arg0] = count(); }
fbt:com.nullmoth.NMXAccel:*nmxSwapKeepFront*:entry { printf("%6d   front %p <- %p\n", (timestamp - t0) / 1000000, arg0, arg1); }
fbt:com.nullmoth.NMXAccel:*nmxCopyForward*:entry { printf("%6d copyfwd mode %d a %p b %p rects %d\n", (timestamp - t0) / 1000000, arg0, arg1, arg2, arg4); }
fbt:com.nullmoth.NMXAccel:*nmxCopyForward*:return { printf("%6d copyfwd -> 0x%x\n", (timestamp - t0) / 1000000, arg1); }
tick-60s { printf("\n--- swaps per surface\n"); printa("  %p: %@d\n", @surf); printf("--- copies to the panel per buffer\n"); printa("  %p: %@d\n", @panel); exit(0); }' 2>&1
} > "$R"
chmod 644 "$R"
rm -f /Library/LaunchDaemons/nmx.swaptrace.plist
launchctl bootout system/nmx.swaptrace 2>/dev/null
RUN
chmod 755 /var/tmp/nmx-swap-trace-run.sh
rm -f /Users/Shared/nmx-swap-trace.txt
launchctl bootout system/nmx.swaptrace 2>/dev/null
cat > /Library/LaunchDaemons/nmx.swaptrace.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>nmx.swaptrace</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>/var/tmp/nmx-swap-trace-run.sh</string></array>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>/var/tmp/nmx-swap-trace.err</string>
</dict></plist>
PL
chown root:wheel /Library/LaunchDaemons/nmx.swaptrace.plist; chmod 644 /Library/LaunchDaemons/nmx.swaptrace.plist
launchctl bootstrap system /Library/LaunchDaemons/nmx.swaptrace.plist && echo "installed - now arm (sudo ./nmx-ws-arm.sh). It records 60 s, starting 10 s after WindowServer restarts." \
  || echo "WARNING: could not start the job"
