#!/bin/bash
# nmx-type-pipeline.sh - typing is late only while armed, yet key -> TextEdit is 3 ms both ways and TextEdit -> next
# swap ~13 ms (10-21). That next swap may be an unrelated one, so this splits the path into steps, each measured from
# the letter to the FIRST event of that step after it:
#   A key in TextEdit (NSTextView insertText)  ->  B TextEdit's CoreAnimation commit
#   -> C WindowServer receives a CoreAnimation commit (QuartzCore *commit_transaction*/Server)
#   -> D WindowServer's display update starts (SkyLight UpdateDisplays)  -> E the frame at the panel copy (submitSwapCopy)
#   (v2: A -> B -> D next display update -> F WindowServer renders the app's layers -> E frame at the panel)
# Run once ARMED and once NOT armed (E only exists armed). Read-only (dtrace, SIP off).
#   sudo ./nmx-type-pipeline.sh          Report: /Users/Shared/nmx-type-pipeline-<armed|unarmed>.txt
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
te=$(pgrep -x TextEdit | head -1); [ -n "$te" ] || { echo "open TextEdit first"; exit 1; }
ws=$(pgrep -x WindowServer | head -1)
st=$([ "$(sysctl -n debug.nvaccelfb 2>/dev/null)" = 1 ] && echo armed || echo unarmed)
R=/Users/Shared/nmx-type-pipeline-$st.txt
echo "($st) Recording 15 s in 3 s - click into the TextEdit document and type steadily until it says done"
sleep 3; echo "NOW - type"
{
echo "=== $(date) typing pipeline ($st), TextEdit $te, WindowServer $ws"
dtrace -q -Z -n "
uint64_t a, f; int wb, wd, wf, we, n, nf, ne;
objc$te:NSTextView:-insertText*:entry { a = timestamp; wb = 1; wd = 1; wf = 1; we = 0; n++; }
pid$te:QuartzCore:*Transaction*commit*:entry /wb && a/ { @b = quantize((timestamp - a) / 1000000); @bv = avg((timestamp - a) / 1000000); wb = 0; }
pid$ws:SkyLight:*UpdateDisplays*:entry /wd && a/ { @d = quantize((timestamp - a) / 1000000); @dv = avg((timestamp - a) / 1000000); wd = 0; }
pid$ws:SkyLight:*WSCALayerBacking*RunUpdate*:entry /wf && a/ { @f = quantize((timestamp - a) / 1000000); @fv = avg((timestamp - a) / 1000000); @fm = max((timestamp - a) / 1000000); f = timestamp; wf = 0; we = 1; nf++; }
pid$ws:SkyLight:*WSCALayerBacking*RunUpdate*:return /we && f/ { @r = quantize((timestamp - f) / 1000); }
fbt:com.nullmoth.NMXAccel:*submitSwapCopy*:entry /we && a/ { @e = quantize((timestamp - a) / 1000000); @ev = avg((timestamp - a) / 1000000); @em = max((timestamp - a) / 1000000); @fe = avg((timestamp - f) / 1000000); we = 0; ne++; }
tick-15s {
  printf(\"letters: %d   WindowServer layer renders after a letter: %d   frames after that: %d\n\", n, nf, ne);
  printf(\"\nA->B  TextEdit commit (ms)\n\"); printa(@b); printa(\"  average %@d\n\", @bv);
  printf(\"\nA->D  next WindowServer display update (ms)\n\"); printa(@d); printa(\"  average %@d\n\", @dv);
  printf(\"\nA->F  WindowServer renders the app's layers (WSCALayerBacking::RunUpdate) (ms)\n\"); printa(@f); printa(\"  average %@d, worst %@d\n\", @fv, @fm);
  printf(\"\n      that layer render itself takes (microseconds)\n\"); printa(@r);
  printf(\"\nA->E  the frame after that render at the panel copy (ms)\n\"); printa(@e); printa(\"  average %@d, worst %@d   (render -> panel average %@d ms)\n\", @ev, @em, @fe);
  exit(0); }" 2>&1
} > "$R"
chmod 644 "$R"
echo "done. Report for me: $R"
grep -E "letters|average" "$R"
