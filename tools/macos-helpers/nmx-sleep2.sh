#!/bin/bash
# nmx-sleep2.sh v2 (b131: + graphics driver / WindowServer at sleep and wake) - why macOS refuses to sleep (no kernel sleep happens; it drops into DarkWake and wakes again).
# Lists every sleep blocker with its owner, then asks for a sleep and records what powerd/the kernel say. Read-only
# apart from the one sleep request.     sudo ./nmx-sleep2.sh      Report: /Users/Shared/nmx-sleep2.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-sleep2.txt
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "--- assertions (who blocks sleep)"; pmset -g assertions 2>&1
  echo "--- kernel sleep blockers"; ioreg -lw0 -r -c IOPMrootDomain 2>/dev/null | grep -iE 'Sleep|Wake|Assertion|Prevent|Hibernate' | grep -v '"IOPowerManagement"' | head -40
  echo "--- hibernation / sleep image"; ls -l /var/vm/sleepimage 2>&1; nvram -p 2>/dev/null | grep -iE 'boot-image|hibernate|IOHibernate' | head
} > $O 2>&1
echo "The Mac will now be asked to sleep in 5 s. Do NOT touch the trackpad, mouse or keyboard for 40 s."
echo "If it really sleeps, wake it with the power button or a key, then wait for this script to finish."
sleep 5
T0=$(date '+%Y-%m-%d %H:%M:%S')
pmset sleepnow >>$O 2>&1
sleep 40
{ echo "--- powerd / kernel messages around the sleep request ($T0)"
  log show --style compact --start "$T0" --predicate '(process == "powerd" OR sender == "IOPMrootDomain" OR eventMessage CONTAINS[c] "PMRD" OR eventMessage CONTAINS[c] "sleep") AND NOT eventMessage CONTAINS "NullMoth step"' 2>/dev/null | grep -iE 'sleep|wake|prevent|cancel|veto|assert|fail|reason|abort|deny' | head -120
  echo "--- pmset log after the request"; pmset -g log 2>/dev/null | grep -E ' (Sleep|Wake|DarkWake|Failure|Kernel Client Acks|Sleep Service) ' | tail -15
  echo "--- sleep counters"; sysctl kern.sleeptime kern.waketime 2>&1
  echo "--- assertions after"; pmset -g assertions 2>&1 | head -40
  echo "--- graphics driver during sleep / wake"
  log show --style compact --start "$T0" --predicate 'eventMessage CONTAINS "NullMoth GPU: power" OR eventMessage CONTAINS "NullMoth GPU: resume" OR eventMessage CONTAINS "NullMoth GPU: suspend" OR eventMessage CONTAINS "NullMoth display: power" OR eventMessage CONTAINS "NullMoth display: backlight" OR eventMessage CONTAINS "NMXAccel: iop: b125" OR (process == "WindowServer" AND (eventMessage CONTAINS[c] "sleep" OR eventMessage CONTAINS[c] "wake" OR eventMessage CONTAINS[c] "power"))' 2>/dev/null | head -80
  echo "--- WindowServer restarts / crashes"
  ls -lt /Library/Logs/DiagnosticReports/ /Users/*/Library/Logs/DiagnosticReports/ 2>/dev/null | grep -iE 'WindowServer|\.panic' | head -5
  echo "nmx-fbpm: $(nvram nmx-fbpm 2>&1)"
} >> $O 2>&1
chmod 644 $O; echo "done - send me $O"
