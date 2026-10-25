#!/bin/bash
# nmx-brave-knobs.sh - b153: which plugin feature breaks Brave's Graphite drawing (white bars, stale text, flicker)?
# Starts Brave several times, each time with some plugin features switched off (environment knobs), and asks you
# whether it still looks bad. No install, no restart. Results: ~/Downloads/nmx-brave-knobs.zip
# Usage: bash nmx-brave-knobs.sh
BRAVE="/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"
OUT="$HOME/Downloads/nmx-brave-knobs"
[ -x "$BRAVE" ] || { echo "Brave not found at $BRAVE"; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT"

quit_brave() { killall "Brave Browser" 2>/dev/null; for i in 1 2 3 4 5 6 7 8 9 10; do pgrep -x "Brave Browser" >/dev/null || return; sleep 1; done; killall -9 "Brave Browser" 2>/dev/null; sleep 1; }

run() {   # run <name> <VAR=VAL ...> [-- <Brave switches ...>]
    local name="$1"; shift
    local envs=() args=()
    while [ $# -gt 0 ] && [ "$1" != "--" ]; do envs+=("$1"); shift; done
    [ "$1" = "--" ] && shift; args=("$@")
    quit_brave
    echo
    echo "=== test $name: ${envs[*]} ${args[*]}"
    env NVMTL_LOG_PATH="$OUT/$name.log" "${envs[@]}" "$BRAVE" --disable-gpu-sandbox "${args[@]}" >/dev/null 2>&1 &
    echo "   Brave is starting. Use it for about 20 seconds:"
    echo "   scroll brave://flags, play a YouTube video, type in a text field."
    local a=""
    while [ "$a" != "b" ] && [ "$a" != "g" ]; do
        read -r -p "   Still WHITE BARS / OVERLAPPING TEXT / FLICKER?  b = still bad,  g = good (fixed): " a
    done
    echo "$name $a ${envs[*]} ${args[*]}" >> "$OUT/results.txt"
    quit_brave
    if grep -q "Brave Browser Helper" "$OUT/$name.log" 2>/dev/null; then echo "   (GPU log ok)"; else echo "   (note: no GPU helper log)"; fi
    LAST="$a"
}

finish() {
    { echo "== results"; cat "$OUT/results.txt"; sw_vers; } > "$OUT/summary.txt"
    (cd "$HOME/Downloads" && rm -f nmx-brave-knobs.zip && zip -qr nmx-brave-knobs.zip nmx-brave-knobs)
    echo; cat "$OUT/results.txt"
    echo; echo "Done. Send ~/Downloads/nmx-brave-knobs.zip"
    echo "Open Brave again with Brave GPU for normal use."
}
[ -n "$NMX_KNOBS_ONLY_DEFS" ] && return 0

ALL=(NVMTL_SURFACE_HOST=0 NVMTL_NO_BUFREUSE=1 NVMTL_NO_RES2=1 NVMTL_NO_SURFACE_PAGEOFF=1
     NVMTL_IMG_RECYCLE=0 NVMTL_SLAB=0 NVMTL_DESC_RECYCLE=0 NVMTL_NO_BUFTEX_RECYCLE=1 NVMTL_NO_HEAPTEX_RECYCLE=1
     NVMTL_NO_POOL_REUSE=1 NVMTL_NO_DIRTYONLY=1 NVMTL_DONTCARE_CLEARS=1)

echo "nmx-brave-knobs: up to 8 short Brave runs. Brave will close and reopen by itself."
run base
run all "${ALL[@]}"
if [ "$LAST" = "g" ]; then
    echo; echo "With everything off it is fixed - now finding which one it is."
    run surface  NVMTL_SURFACE_HOST=0 NVMTL_NO_SURFACE_PAGEOFF=1 NVMTL_NO_DIRTYONLY=1
    run bufreuse NVMTL_NO_BUFREUSE=1 NVMTL_NO_POOL_REUSE=1
    run residency NVMTL_NO_RES2=1
    run recycle  NVMTL_IMG_RECYCLE=0 NVMTL_SLAB=0 NVMTL_DESC_RECYCLE=0 NVMTL_NO_BUFTEX_RECYCLE=1 NVMTL_NO_HEAPTEX_RECYCLE=1
    run dontcare NVMTL_DONTCARE_CLEARS=1
fi
finish
