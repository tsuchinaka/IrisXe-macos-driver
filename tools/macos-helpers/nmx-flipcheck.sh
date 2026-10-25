#!/bin/bash
# nmx-flipcheck.sh (b125) - why the vblank flip is laggy. Turns the flip ON for 15 s while you animate, measures how long
# each frame spends in copy + flip (the wait for vblank included) and how many frames reach the panel, then turns it OFF
# again and does the same 15 s with the copy, for comparison.   sudo ./nmx-flipcheck.sh   Report: /Users/Shared/nmx-flipcheck.txt
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
O=/Users/Shared/nmx-flipcheck.txt
g() { sysctl -n debug.$1 2>/dev/null || echo 0; }
run() {  # $1 = 1 flip / 0 copy
  sysctl -w debug.nvaccel_iop_flipdb=$1 >/dev/null
  local ok0=$(g nvaccel_iop_ok) f0=$(g nvaccel_iop_dbflips) n0=$(g nvaccel_iop_dbflip_ns) x0=$(g nvaccel_iop_dbflip_fail) c0=$(g nvaccel_iop_copies) cn0=$(g nvaccel_iop_copy_ns) s0=$(g nvaccel_iop_settles)
  sleep 15
  local ok1=$(g nvaccel_iop_ok) f1=$(g nvaccel_iop_dbflips) n1=$(g nvaccel_iop_dbflip_ns) x1=$(g nvaccel_iop_dbflip_fail) c1=$(g nvaccel_iop_copies) cn1=$(g nvaccel_iop_copy_ns) s1=$(g nvaccel_iop_settles)
  echo "  frames: $((ok1-ok0)) in 15 s ($(( (ok1-ok0)/15 ))/s)   flips: $((f1-f0))   flip refused: $((x1-x0))   settle copies: $((s1-s0))"
  [ $((f1-f0)) -gt 0 ] && echo "  copy+flip per frame: $(( (n1-n0)/(f1-f0)/1000 )) us average"
  [ $((c1-c0)) -gt 0 ] && echo "  transaction time per frame (copy or flip): $(( (cn1-cn0)/(c1-c0)/1000 )) us average"
  echo "  WindowServer CPU: $(ps -o %cpu= -p $(pgrep -x WindowServer | head -1))"
}
{ echo "=== $(date)  boot-args: $(sysctl -n kern.bootargs)"
  echo "ANIMATE the whole 30 s (Mission Control, scroll, drag windows)" >&2
  echo "--- FLIP ON (15 s)"; run 1
  echo "--- FLIP OFF / copy (15 s)"; run 0
  echo "--- counters"; sysctl debug 2>/dev/null | grep -E 'nvaccel_iop_(settle|dbflip|flipdb)'
  echo "--- kernel log (flip)"; log show --last 2m --style compact --predicate 'eventMessage CONTAINS "flip"' 2>/dev/null | tail -20
} > $O 2>&1
chmod 644 $O; echo "done - send me $O"
