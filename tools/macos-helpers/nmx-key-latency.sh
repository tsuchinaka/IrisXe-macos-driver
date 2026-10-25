#!/bin/bash
# nmx-key-latency.sh - typing is delayed only while ARMED (10-21), and from TextEdit to the panel takes only ~13 ms
# (nmx-type-latency.sh). So the time is lost BEFORE TextEdit gets the key. 15 s of dtrace while you type in TextEdit:
#   key -> TextEdit    : the keyboard event in the kernel (dispatchKeyboardEvent) until TextEdit's keyDown:
#   surface lock waits : how long WindowServer waits in the kernel for the screen buffer (surface_lock_options), the
#                        same thread that hands keyboard events to apps (lag probe 10-20: most of its display time)
# Run it once armed and once NOT armed, then send both reports. Read-only (dtrace, SIP off).
#   sudo ./nmx-key-latency.sh          Report: /Users/Shared/nmx-key-latency-<armed|unarmed>.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
te=$(pgrep -x TextEdit | head -1); [ -n "$te" ] || { echo "open TextEdit first"; exit 1; }
ws=$(pgrep -x WindowServer | head -1)
st=$([ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ] && echo armed || echo unarmed)
R=/Users/Shared/nmx-key-latency-$st.txt
echo "($st) Recording 15 s in 3 s - click into the TextEdit document and type steadily until it says done"
sleep 3; echo "NOW - type"
{
echo "=== $(date) key latency ($st), TextEdit $te, WindowServer $ws"
dtrace -q -Z -n "
uint64_t k; int keys, downs;
fbt::*dispatchKeyboardEvent*:entry { k = timestamp; keys++; }
objc$te:NSTextView:-keyDown?:entry /k/ { @kd = quantize((timestamp - k) / 1000000); @kdavg = avg((timestamp - k) / 1000000); @kdmax = max((timestamp - k) / 1000000); downs++; k = 0; }
fbt:com.apple.iokit.IOAcceleratorFamily2:*surface_lock_options*:entry /pid == $ws/ { self->l = timestamp; }
fbt:com.apple.iokit.IOAcceleratorFamily2:*surface_lock_options*:return /self->l/ { @lock = quantize((timestamp - self->l) / 1000); @locksum = sum((timestamp - self->l) / 1000); @lockn = count(); self->l = 0; }
tick-15s {
  printf(\"keyboard events in the kernel: %d   keyDown in TextEdit: %d\n\", keys, downs);
  printf(\"\n--- key (kernel) -> TextEdit keyDown (ms)\n\"); printa(@kd); printa(\"average %@d ms, worst %@d ms\n\", @kdavg, @kdmax);
  printf(\"\n--- WindowServer waiting for the screen buffer, per lock (microseconds)\n\"); printa(@lock);
  printa(\"locks %@d, total wait %@d us in 15 s\n\", @lockn, @locksum);
  exit(0); }" 2>&1
} > "$R"
chmod 644 "$R"
echo "done. Report for me: $R"
grep -E "keyboard events|average|total wait" "$R"
