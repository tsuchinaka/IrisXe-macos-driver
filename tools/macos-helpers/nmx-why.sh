#!/bin/bash
# Why didn't NMXGPU come up? Saves everything to /Users/Shared/nmx-why.txt
OUT=/Users/Shared/nmx-why.txt
{
echo "== date"; date
echo "== boot-args"; nvram boot-args 2>&1
echo "== uptime"; uptime
echo "== loaded NMX/NVRM kexts"; kmutil showloaded 2>/dev/null | grep -iE "nmx|nvrm|nvmtl|lilu|whatevergreen|green" 
echo "== kexts on disk"; ls -ld /Library/Extensions/NMX* /Library/Extensions/NVRM* 2>&1
echo "== aux collection"; kmutil showloaded --collection aux 2>&1 | head -40
echo "== aux contents"; kmutil inspect --aux 2>&1 | grep -iE "nmx|nvrm"
echo "== IOReg GPU"; ioreg -rlw0 -c IOPCIDevice -n GFX0 2>/dev/null | grep -E "class|IOName|built-in|NMX|device-id|vendor-id" | head -20
echo "== IOReg framebuffers"; ioreg -lw0 -c IOFramebuffer 2>/dev/null | grep -E "\+-o|IOFBCurrentPixel|NMX" | head -20
echo "== kernel log this boot (NMX/i915/drm)"
log show --last boot --predicate 'sender CONTAINS "NMX" OR eventMessage CONTAINS[c] "nmx" OR eventMessage CONTAINS[c] "i915" OR eventMessage CONTAINS "[drm"' --style compact 2>/dev/null | head -400
echo "== kext load errors this boot"
log show --last boot --predicate 'eventMessage CONTAINS[c] "kext" AND (eventMessage CONTAINS[c] "fail" OR eventMessage CONTAINS[c] "error" OR eventMessage CONTAINS[c] "not loadable" OR eventMessage CONTAINS[c] "reject")' --style compact 2>/dev/null | head -80
echo "== panics"; ls -lt /Library/Logs/DiagnosticReports/ 2>/dev/null | grep -iE "panic|kernel" | head
} > "$OUT" 2>&1
chmod 644 "$OUT"
echo "Saved: $OUT  ($(wc -l < "$OUT") lines)"
