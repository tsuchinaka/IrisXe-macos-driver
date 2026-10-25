#!/bin/bash
# nmx-brave-dawn.sh - b153 round 2: Brave's Graphite drawing (white bars, stale text, flicker) with Dawn switched to
# safer paths. Round 1 (nmx-brave-knobs.sh): every plugin memory/reuse trick off - still bad, so those are cleared.
#   vpull  - Dawn fetches vertices in its own shader code instead of Metal vertex descriptors (tests our vertex fetch)
#   norob  - Dawn without its bounds-clamping code (tests the buffer sizes our plugin reports to shaders)
# Usage: bash nmx-brave-dawn.sh      Results: ~/Downloads/nmx-brave-knobs.zip
HERE="$(cd "$(dirname "$0")" && pwd)"
export NMX_KNOBS_ONLY_DEFS=1
source "$HERE/nmx-brave-knobs.sh"
echo "nmx-brave-dawn: 3 short Brave runs. Brave will close and reopen by itself."
run vpull -- --enable-dawn-features=metal_enable_vertex_pulling
run norob -- --enable-dawn-features=disable_robustness
run both  -- --enable-dawn-features=metal_enable_vertex_pulling,disable_robustness
finish
