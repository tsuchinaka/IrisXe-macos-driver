#!/bin/bash
# nmx-power.sh [seconds] - how much power the laptop draws at idle, measured by the battery (SimpleACPIBattery).
#
#   1. unplug the charger   2. leave the desktop idle (screen on, no apps busy)   3. ./nmx-power.sh 120
#
# Changes nothing. Samples the battery's discharge (Amperage x Voltage, refreshed by the battery every ~15 s) for the given
# time (default 120 s), prints the average in watts, plus the GPU's power state from this boot's log (RC6 / GT parking,
# clock governor, last GPU clock reports). Writes ~/Desktop/nmx-power.txt. Compare runs with the same screen brightness.
SECS=${1:-120}
OUT="$HOME/Desktop/nmx-power.txt"
[ "$(id -u)" = 0 ] && [ -n "$SUDO_USER" ] && OUT="/Users/$SUDO_USER/Desktop/nmx-power.txt"
val() { ioreg -r -c IOPMPowerSource -l 2>/dev/null | sed -n "s/.*\"$1\" = \\(-*[0-9]*\\).*/\\1/p" | head -1; }
{
echo "=== power at idle $(date), ${SECS} s"
ext=$(ioreg -r -c IOPMPowerSource -l 2>/dev/null | sed -n 's/.*"ExternalConnected" = \([A-Za-z]*\).*/\1/p' | head -1)
[ "$ext" = "Yes" ] && echo "WARNING: the charger is connected - unplug it, the battery reading is not the laptop's draw then"
n=0; sum=0
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
    a=$(val Amperage); v=$(val Voltage)
    if [ -n "$a" ] && [ -n "$v" ]; then
        # ioreg prints a negative (discharging) amperage as an unsigned 64-bit number: 2^64 - x
        [ ${#a} -ge 19 ] && a=$(echo "18446744073709551616 - $a" | bc)   # exact (awk's doubles cannot do it)
        a=${a#-}
        w=$(awk -v a="$a" -v v="$v" 'BEGIN{ printf "%.2f", a * v / 1000000 }')
        echo "$(date +%T)  ${a} mA  ${v} mV  ${w} W"
        sum=$(awk -v s="$sum" -v w="$w" 'BEGIN{ printf "%.4f", s + w }'); n=$((n + 1))
    else
        echo "$(date +%T)  no battery reading (is SimpleACPIBattery loaded?)"
    fi
    sleep 5
done
[ $n -gt 0 ] && echo "AVERAGE: $(awk -v s="$sum" -v n="$n" 'BEGIN{ printf "%.2f", s / n }') W over $n samples"
echo
echo "--- GPU power state (this boot)"
log show --last boot --style compact --predicate 'eventMessage CONTAINS "RC6" OR eventMessage CONTAINS "keep-awake" OR eventMessage CONTAINS "clock governor"' 2>/dev/null | grep -v "^Timestamp" | tail -8 | cut -c1-200
echo "--- last GPU clock reports"
log show --last 3m --style compact --predicate 'eventMessage CONTAINS "GPU clock min"' 2>/dev/null | grep -v "^Timestamp" | tail -4 | sed 's/.*VBL:/VBL:/' | cut -c1-200
echo "--- screen brightness: $(log show --last 10m --style compact --predicate 'eventMessage CONTAINS "backlight: "' 2>/dev/null | grep -o 'backlight: [0-9]*%' | tail -1)"
} | tee "$OUT"
echo "saved: $OUT"
