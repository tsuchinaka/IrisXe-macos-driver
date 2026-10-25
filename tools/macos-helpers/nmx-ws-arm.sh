#!/bin/bash
# nmx-ws-arm.sh - turn on GPU acceleration for the whole desktop (WindowServer) for THIS BOOT ONLY.
# A restart always undoes it. Run: sudo ./nmx-ws-arm.sh
[ "$(id -u)" = 0 ] || { echo "run with: sudo ./nmx-ws-arm.sh"; exit 1; }
LOG=/var/log/nmx-ws.log; exec > >(tee -a "$LOG") 2>&1
echo "=== $(date) arm"
grep -q "nmxws=1" <<<"$(sysctl -n kern.bootargs)" || { echo "STOP: boot-arg nmxws=1 is missing - boot with config-Sequoia-NMX-accel-ws.plist first"; exit 1; }
sysctl debug.nvaccelfb >/dev/null 2>&1 || { echo "STOP: debug.nvaccelfb is missing - the new NMXAccel.kext (build 75) is not loaded"; exit 1; }
touch /tmp/nvmtl.log; chmod 666 /tmp/nvmtl.log
A=/Library/GPUBundles/nvmtl-allow.txt
grep -qx '!WindowServer' "$A" 2>/dev/null || echo '!WindowServer' >> "$A"
# b104 (Phase 4): an app test (nmx-app-gpu.sh) that never finished froze the Mac - take those apps off the list first,
# or macOS reopening them at login would put them straight back on the GPU
apps=$(nvram nmx-app-test 2>/dev/null | cut -f2-)
if [ -n "$apps" ]; then
  IFS='|' read -r -a bad <<<"$apps"
  for n in "${bad[@]}"; do [ -n "$n" ] && { grep -vxF "!$n" "$A" > "$A.tmp"; cat "$A.tmp" > "$A"; rm -f "$A.tmp"; }; done
  nvram -d nmx-app-test; echo "$(date) $apps froze the Mac (removed by nmx-ws-arm)" >> /Users/Shared/nmx-app-frozen.txt
  echo "app test guard: took off the GPU list (it froze the Mac last time): $apps"
fi
echo "allow-list:"; cat "$A"
sysctl -w debug.nvaccelfb=1; sleep 1
# b112: NMXAccel refuses the arm while WindowServer has not opened the screen (it used to panic) - stop here then
[ "$(sysctl -n debug.nvaccelfb)" = 1 ] || { echo "STOP: the accelerator refused to arm (the screen is not open yet?) - see: log show --last 1m --predicate 'eventMessage CONTAINS \"REFUSED\"'"; exit 1; }
# b87: level 4 right after 1, exactly as the NVIDIA driver arms itself at boot (NVRM autoGo): opens the scan-out gate, so
# the display pipe can back WindowServer's framebuffer resource with the real screen (it answered NULL at level 1)
if [ "$(sysctl -n debug.nvaccelfb)" = 1 ]; then sysctl -w debug.nvaccelfb=4; sleep 1; fi
# b84: the AppleGraphicsDeviceControl object (NVRMAGDC.kext) - without it WindowServer's IOPresentment never sends frames
if sysctl debug.nvrmfb_agdc >/dev/null 2>&1; then
  # b86: answer the link-configuration query (0x921) from the panel timing NMXGPUFB publishes (NVRMTiming)
  sysctl -w debug.nvrmfb_agdc_k5=1
  ioreg -r -c NMXGPUFB -l 2>/dev/null | grep -q NVRMTiming && echo "panel timing: published" || echo "WARNING: NMXGPUFB has no NVRMTiming - install NMXGPU build 86 and boot the ws config"
  sysctl -w debug.nvrmfb_agdc=1
  for i in 1 2 3 4 5 6 7 8 9 10; do [ "$(sysctl -n debug.nvrmfb_agdc)" = 1 ] && break; sleep 0.5; done
  echo "AGDC state: $(sysctl -n debug.nvrmfb_agdc) (1 = up)"
else
  echo "WARNING: NVRMAGDC.kext is not loaded - install it (build 84) or WindowServer will not show frames"
fi
sysctl -w debug.nvaccel_iop=1
sysctl debug.nvaccelfb debug.nvaccel_iop debug.nvaccel_heads_published debug.nvaccel_heads_piped
if [ "$(sysctl -n debug.nvaccelfb)" != 1 ]; then echo "STOP: the screen did not register with the accelerator (debug.nvaccelfb is not 1). Send me $LOG"; exit 1; fi
echo "armed. Restarting WindowServer in 5 seconds - you will be logged out. Log in again, then run: sudo ./nmx-ws-check.sh"
echo "If the screen stays black for 1 minute: hold the power button, restart - everything goes back to normal."
# b86: an automatic report 30 s and 45 s after the restart, saved to /Users/Shared/nmx-ws-auto.txt (runs from launchd,
# so it survives the logout; no SSH needed)
cat > /var/tmp/nmx-ws-auto.sh <<'AUTO'
#!/bin/bash
R=/Users/Shared/nmx-ws-auto.txt
C="debug.nvaccelfb debug.nvaccel_iop debug.nvaccel_iop_ok debug.nvaccel_iop_fail debug.nvaccel_iop_empty debug.nvaccel_iop_blank debug.nvaccel_iop_src_surf debug.nvaccel_heads_piped debug.nvrmfb_agdc debug.nvrmfb_agdc_cmds debug.nvrmfb_agdc_refused debug.nvrmfb_agdc_last_refused debug.nvrmfb_agdc_k5 debug.nvrmfb_agdc_k5_link debug.nvrmfb_agdc_k5_caps debug.nvrmfb_agdc_k5_why debug.nvrmfb_agdc_k5_asked"
sleep 30
{ echo "=== $(date) (30 s after the WindowServer restart)"; echo "WindowServer pid $(pgrep -x WindowServer | head -1)"; sysctl $C 2>&1; } > $R
sleep 15
{ echo "=== $(date) (45 s)"; sysctl debug.nvaccel_iop_ok debug.nvaccel_iop_fail debug.nvaccel_iop_empty debug.nvaccel_iop_blank 2>&1
  echo "--- kernel"; log show --last 2m --style compact --predicate 'sender == "NVRMAGDC" OR sender == "NMXAccel" OR sender == "NMXGPU"' 2>/dev/null | grep -v -e "NVSurface::" -e "getDirtyBuffer" -e "DISP\[" -e "getStampMemory" -e "createUserGPUTask" | tail -80
  echo "--- WindowServer"; log show --last 2m --style compact --predicate 'process == "WindowServer" AND (eventMessage CONTAINS "IOPresentment" OR eventMessage CONTAINS "display pipe" OR eventMessage CONTAINS[c] "error")' 2>/dev/null | tail -40
  echo "--- metal log"; tail -30 "$(find /var/folders -name nvmtl-ws.log 2>/dev/null | head -1)" 2>&1; } >> $R
chmod 644 $R
rm -f /Library/LaunchDaemons/nmx.wsauto.plist; launchctl bootout system/nmx.wsauto 2>/dev/null
AUTO
chmod 755 /var/tmp/nmx-ws-auto.sh
rm -f /Users/Shared/nmx-ws-auto.txt
# a system LaunchDaemon (not `launchctl submit`, which lives in the login session and dies with the logout)
launchctl bootout system/nmx.wsauto 2>/dev/null
cat > /Library/LaunchDaemons/nmx.wsauto.plist <<'PL'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>nmx.wsauto</string>
<key>ProgramArguments</key><array><string>/bin/bash</string><string>/var/tmp/nmx-ws-auto.sh</string></array>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>/var/tmp/nmx-ws-auto.err</string>
</dict></plist>
PL
chown root:wheel /Library/LaunchDaemons/nmx.wsauto.plist; chmod 644 /Library/LaunchDaemons/nmx.wsauto.plist
if launchctl bootstrap system /Library/LaunchDaemons/nmx.wsauto.plist; then
  echo "automatic report: /Users/Shared/nmx-ws-auto.txt in about 50 seconds"
else
  echo "WARNING: the automatic report could not start - run sudo ./nmx-ws-check.sh over SSH instead"
fi
sleep 5; killall -9 WindowServer
