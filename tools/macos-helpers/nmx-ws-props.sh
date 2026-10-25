#!/bin/bash
# nmx-ws-props.sh - b116: with the modern classes (nmxmodern=1) WindowServer still never opens the display pipe
# (accelerator user client type 4 = IOAccelDisplayPipeUserClient2; 0x20 = IOAccelSurfaceMTL) - it only opens types
# 0/5/6. Something it reads first decides that. This arms exactly like nmx-ws-arm.sh and records, for the NEW
# WindowServer from its first moment, every IORegistry property it asks for (and which are MISSING) and every user
# client type it opens. Read-only kernel dtrace (SIP off), runs from a LaunchDaemon so it survives the logout.
#   sudo ./nmx-ws-props.sh        (instead of nmx-ws-arm.sh)   Report after ~70 s: /Users/Shared/nmx-ws-props.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
D=$(cd "$(dirname "$0")" && pwd)
[ -x "$D/nmx-ws-arm.sh" ] || { echo "nmx-ws-arm.sh must be in the same folder"; exit 1; }
cat > /var/tmp/nmx-ws-props.d <<'DT'
#pragma D option quiet
#pragma D option strsize=128
fbt::is_io_registry_entry_get_property:entry,
fbt::is_io_registry_entry_get_property_bytes:entry
/execname == "WindowServer"/ { self->n = stringof(arg1); self->in = 1; }
fbt::is_io_registry_entry_get_property_bin:entry,
fbt::is_io_registry_entry_get_property_bin_buf:entry,
fbt::is_io_registry_entry_get_property_recursively:entry
/execname == "WindowServer"/ { self->n = stringof(arg2); self->in = 1; }
fbt::is_io_registry_entry_get_property:return,
fbt::is_io_registry_entry_get_property_bytes:return,
fbt::is_io_registry_entry_get_property_bin:return,
fbt::is_io_registry_entry_get_property_bin_buf:return,
fbt::is_io_registry_entry_get_property_recursively:return
/self->in/ { @q[self->n, arg1 == 0 ? "found" : "MISSING"] = count(); self->in = 0; }
fbt::is_io_service_open_extended:entry /execname == "WindowServer"/ { @open[arg2] = count(); }
/* v3: the accelerator's own side - which user-client types the family is asked for (type 4 = display pipe), and
   every display-pipe user-client call WindowServer makes, with its return value */
fbt:com.apple.iokit.IOAcceleratorFamily2:_ZN22IOGraphicsAccelerator213newUserClientEP4taskPvjPP12IOUserClient:entry
/execname == "WindowServer"/ { @acc[arg3] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipeUserClient2*:return /execname == "WindowServer"/ { @dp[probefunc, arg1] = count(); }
fbt:com.apple.iokit.IOAcceleratorFamily2:*IOAccelDisplayPipe*transaction*:entry /execname == "WindowServer"/ { @tx[probefunc] = count(); }
fbt::is_io_registry_entry_create_iterator:entry,
fbt::is_io_service_get_matching_services_bin:entry /execname == "WindowServer"/ { @calls[probefunc] = count(); }
tick-65s {
  printf("=== user client types WindowServer opened (type, count): 0 surface, 4 DISPLAY PIPE, 5 device, 6 shared, 0x20 Metal surface\n"); printa("  %d %@d\n", @open);
  printf("\n=== accelerator user clients WindowServer asked the family for (type: 0 surface, 4 DISPLAY PIPE, 5 device, 6 shared, 32 Metal surface)\n"); printa("  %d %@d\n", @acc);
  printf("\n=== display-pipe user client calls (function, return value, count)\n"); printa("  %-90s 0x%x %@d\n", @dp);
  printf("\n=== display-pipe transaction calls\n"); printa("  %s %@d\n", @tx);
  printf("\n=== properties WindowServer asked for (name, result, count)\n"); printa("  %-50s %-8s %@d\n", @q);
  printf("\n"); printa("  %s %@d\n", @calls);
  exit(0); }
DT
cat > /var/tmp/nmx-ws-props-run.sh <<'RUN'
#!/bin/bash
# v2: write to /var/tmp first (the report is copied to /Users/Shared at the end), leave a marker at once
date > /var/tmp/nmx-ws-props.started
R=/var/tmp/nmx-ws-props.txt
OLD=$(pgrep -x WindowServer | head -1)
# v4: the moment the NEW WindowServer exists, attach to it and count which compositor / Metal / presentment functions
# of SkyLight, QuartzCore and CoreDisplay it runs (which compositor it picks, and why not the Metal one)
( for i in $(seq 1 600); do N=$(pgrep -x WindowServer | head -1); [ -n "$N" ] && [ "$N" != "$OLD" ] && break; sleep 0.05; done
  [ -n "$N" ] && [ "$N" != "$OLD" ] && dtrace -Z -q -p "$N" -n '
pid$target:SkyLight:*ompositor*:entry, pid$target:SkyLight:*Metal*:entry, pid$target:SkyLight:*IOPresent*:entry,
pid$target:SkyLight:*DisplayPipe*:entry, pid$target:SkyLight:*CanUse*:entry, pid$target:SkyLight:*Supports*:entry,
pid$target:QuartzCore:*MetalContext*:entry, pid$target:QuartzCore:*SWContext*:entry, pid$target:QuartzCore:*metal_context*:entry,
pid$target:CoreDisplay:*Presentment*:entry, pid$target:CoreDisplay:*Metal*:entry, pid$target:CoreDisplay:*ompositor*:entry
{ @f[probemod, probefunc] = count(); }
tick-40s { printf("=== WindowServer %d: compositor / Metal / presentment functions it ran in its first 40 s (module, function, count)\n", $target); printa("  %-12s %-120s %@d\n", @f); exit(0); }' > /var/tmp/nmx-ws-props-user.txt 2>&1 ) &
{ echo "=== $(date) WindowServer property trace v4, boot-args: $(sysctl -n kern.bootargs)"
  dtrace -Z -s /var/tmp/nmx-ws-props.d 2>&1
  wait
  echo; cat /var/tmp/nmx-ws-props-user.txt 2>&1
  echo "--- after: iop_ok $(sysctl -n debug.nvaccel_iop_ok 2>/dev/null)"
  echo "--- NMXGPUFB properties"; ioreg -r -c NMXGPUFB -d 1 -l 2>/dev/null | grep -E '"IOAccel|"IOFB|NMXFB|IOGraphics|AAPL' | head -60
  echo "--- NMXAccel properties"; ioreg -r -c NMXAccel -d 1 -l 2>/dev/null | grep -E '"IOAccel|Metal|IOGL|IOCFPlugIn|IOVA|Perf' | head -40
} > $R 2>&1
cp $R /Users/Shared/nmx-ws-props.txt; chmod 644 /Users/Shared/nmx-ws-props.txt
launchctl bootout system/nmx.wsprops 2>/dev/null; rm -f /Library/LaunchDaemons/nmx.wsprops.plist
RUN
chmod 755 /var/tmp/nmx-ws-props-run.sh
rm -f /Users/Shared/nmx-ws-props.txt /var/tmp/nmx-ws-props.txt /var/tmp/nmx-ws-props.started /var/tmp/nmx-ws-props.err
cat > /Library/LaunchDaemons/nmx.wsprops.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>nmx.wsprops</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>/var/tmp/nmx-ws-props-run.sh</string></array>
<key>RunAtLoad</key><true/>
<key>AbandonProcessGroup</key><true/>
<key>StandardOutPath</key><string>/var/tmp/nmx-ws-props.err</string>
<key>StandardErrorPath</key><string>/var/tmp/nmx-ws-props.err</string>
</dict></plist>
PL
chown root:wheel /Library/LaunchDaemons/nmx.wsprops.plist; chmod 644 /Library/LaunchDaemons/nmx.wsprops.plist
launchctl bootout system/nmx.wsprops 2>/dev/null
launchctl bootstrap system /Library/LaunchDaemons/nmx.wsprops.plist || { echo "could not start the trace"; exit 1; }
sleep 3
[ -f /var/tmp/nmx-ws-props.started ] || { echo "STOP: the trace did not start - send me: cat /var/tmp/nmx-ws-props.err; launchctl print system/nmx.wsprops"; exit 1; }
echo "trace running; now arming (you will be logged out). Log in again and wait ~70 s, then send /Users/Shared/nmx-ws-props.txt"
exec "$D/nmx-ws-arm.sh"
