#!/bin/bash
# nmx-brave-intel.sh - b153 round 3: Brave's Graphite drawing (white bars, stale text, flicker) with the Intel Vulkan
# driver's (ANV, Mesa) image compression tricks switched off through INTEL_DEBUG.
# Rounds 1-2 cleared our plugin's memory/reuse tricks, vertex fetch and Dawn's bounds clamping.
#   nofc  - no fast clears        noccs - no colour compression (CCS)        nohiz - no depth compression (HiZ)
#   sync  - wait for the GPU after every batch (timing test)
# Usage: bash nmx-brave-intel.sh      Results: ~/Downloads/nmx-brave-knobs.zip
HERE="$(cd "$(dirname "$0")" && pwd)"
export NMX_KNOBS_ONLY_DEFS=1
source "$HERE/nmx-brave-knobs.sh"
echo "nmx-brave-intel: 2 to 5 short Brave runs. Brave will close and reopen by itself."
run alloff INTEL_DEBUG=nofc,noccs,nohiz
if [ "$LAST" = "g" ]; then
    echo; echo "With all three off it is fixed - now finding which one it is."
    run nofc  INTEL_DEBUG=nofc
    run noccs INTEL_DEBUG=noccs
    run nohiz INTEL_DEBUG=nohiz
else
    run sync  INTEL_DEBUG=sync
fi
finish
