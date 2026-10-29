#!/bin/bash
# nmx-hidpi.sh status|on|off - sharper text and icons: HiDPI ("Retina") desktop sizes for the built-in panel (b206).
#
#   sudo ./nmx-hidpi.sh status   shows the panel's display IDs, the mode now in use and whether the override is there
#   sudo ./nmx-hidpi.sh on       adds HiDPI sizes for the panel; restart, then System Settings -> Displays
#   sudo ./nmx-hidpi.sh off      removes them again; restart
#
# Why: the driver's smaller desktop sizes (1920, 1680, 1600, 1440, 1280 wide) are real 1x modes - macOS draws the
# desktop that small and the display engine stretches it to the 2560x1600 panel, so every pixel becomes ~1.5 pixels and
# text is soft. With acceleration on, WindowServer can instead draw every point as 2x2 pixels ("HiDPI", as on a real
# MacBook) and scale that to the panel on the GPU. "1280 x 800 HiDPI" is exactly the panel's pixels (sharpest, no
# scaling); 1440x900 / 1600x1000 / 1680x1050 HiDPI give more space and are scaled down on the GPU (more GPU work).
#
# How: a display override file (scale-resolutions), the same mechanism macOS uses for Apple's own displays, in
# /Library/Displays/Contents/Resources/Overrides/DisplayVendorID-<v>/DisplayProductID-<p>. Nothing in the driver changes.
# If the screen is wrong after the restart: boot the other macOS (or Recovery) and delete that file, or run 'off'.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
OV=/Library/Displays/Contents/Resources/Overrides

# the built-in panel's IDs as IODisplay sees them (decimal in ioreg; the folder names use hex without leading zeros)
ids() {
    local io; io=$(ioreg -lw0 -r -c IODisplayConnect 2>/dev/null)
    VID=$(awk -F'= ' '/"DisplayVendorID"/{print $2; exit}' <<<"$io")
    PID=$(awk -F'= ' '/"DisplayProductID"/{print $2; exit}' <<<"$io")
    if [ -z "$VID" ] || [ -z "$PID" ]; then echo "ERROR: no display IDs found (ioreg IODisplayConnect)"; exit 1; fi
    VHEX=$(printf '%x' "$VID"); PHEX=$(printf '%x' "$PID")
    DIR="$OV/DisplayVendorID-$VHEX"; FILE="$DIR/DisplayProductID-$PHEX"
}

# one scale-resolutions entry: width, height, flags, 0x00200000 (big-endian, as in Apple's own override files);
# flags 1 = HiDPI (the width/height given are the pixel size, 2x the "looks like" size)
entry() {
    local w=$1 h=$2 f=$3
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((w>>24&255)) $((w>>16&255)) $((w>>8&255)) $((w&255)))"
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((h>>24&255)) $((h>>16&255)) $((h>>8&255)) $((h&255)))"
    printf "$(printf '\\x%02x\\x%02x\\x%02x\\x%02x' $((f>>24&255)) $((f>>16&255)) $((f>>8&255)) $((f&255)))"
    printf '\x00\x20\x00\x00'
}
b64() { entry "$@" | base64 | tr -d '\n'; }

case "${1:-status}" in
status)
    ids
    echo "panel IDs: vendor $VID (0x$VHEX), product $PID (0x$PHEX)"
    echo "override:  $([ -f "$FILE" ] && echo "ON  ($FILE)" || echo "off")"
    echo "--- what macOS shows now"
    system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Resolution|UI Looks like|Retina|Main Display|Display Type' | sed 's/^ */  /'
    ;;
on)
    ids
    mkdir -p "$DIR"
    {
    echo '<?xml version="1.0" encoding="UTF-8"?>'
    echo '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">'
    echo '<plist version="1.0"><dict>'
    echo "  <key>DisplayProductName</key><string>Built-in Retina Display</string>"
    echo "  <key>DisplayVendorID</key><integer>$VID</integer>"
    echo "  <key>DisplayProductID</key><integer>$PID</integer>"
    echo '  <key>scale-resolutions</key><array>'
    # "looks like" sizes, all 16:10 like the panel: pixel size = 2x
    for wh in 1280x800 1440x900 1600x1000 1680x1050; do
        w=${wh%x*}; h=${wh#*x}
        echo "    <data>$(b64 $((w*2)) $((h*2)) 1)</data>"
    done
    echo '  </array>'
    echo '</dict></plist>'
    } > "$FILE"
    chmod 644 "$FILE"; chown root:wheel "$FILE"
    if ! plutil -lint "$FILE" >/dev/null; then echo "ERROR: the file did not come out right - removed"; rm -f "$FILE"; exit 1; fi
    echo "HiDPI sizes added: $FILE"
    echo
    echo "Now:"
    echo "  1. Restart the Mac."
    echo "  2. System Settings -> Displays: pick a size. The sharpest is the one that looks like 1280 x 800"
    echo "     (exactly the panel's pixels). 1440 x 900 gives more space and is still sharp."
    echo "     (If Displays shows only a list, turn on 'Show all resolutions' and pick one marked HiDPI.)"
    echo "  3. If nothing changed, send me: sudo $0 status"
    ;;
off)
    ids
    rm -f "$FILE"; rmdir "$DIR" 2>/dev/null
    echo "HiDPI override removed - restart the Mac"
    ;;
*) echo "usage: sudo $0 status|on|off"; exit 1 ;;
esac
