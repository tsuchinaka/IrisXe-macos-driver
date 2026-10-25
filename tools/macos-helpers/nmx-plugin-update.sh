#!/bin/bash
# nmx-plugin-update.sh - update the Metal plugin WITHOUT deleting the parts the kext build does not make.
#
#   sudo ./nmx-plugin-update.sh <path to new NVMTLDriver.bundle>                (from the nmxgpu-intel-build artifact)
#   sudo ./nmx-plugin-update.sh <new NVMTLDriver.bundle> <full NVMTLDriver.bundle> (also restore air-runtime from the
#        "Mesa ANV + Metal plugin" artifact: nmx-vulkan-.../out/nmx-metal/NVMTLDriver.bundle)
#
# The nmxgpu-intel-build workflow builds only the plugin binary (Contents/MacOS/NVMTLDriver). The shader reader
# (Contents/MacOS/air-runtime/libLLVM.dylib + its libraries) comes only from mesa-anv-build.yml. Replacing the whole
# bundle deletes it, and then every shader that is not cached fails ("air-opt failed (wait status 6)") - so this script
# replaces ONLY the binary. A WindowServer restart (nmx-ws-arm.sh) loads the new plugin.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
NEW=${1:?usage: sudo $0 <new NVMTLDriver.bundle> [<full NVMTLDriver.bundle with air-runtime>]}
FULL=${2:-}
DST=/Library/GPUBundles/NVMTLDriver.bundle
BIN="$NEW/Contents/MacOS/NVMTLDriver"
[ -f "$BIN" ] || { echo "STOP: no $BIN"; exit 1; }
mkdir -p "$DST/Contents/MacOS"
[ -f "$DST/Contents/MacOS/NVMTLDriver" ] && cp -p "$DST/Contents/MacOS/NVMTLDriver" "/Library/GPUBundles/NVMTLDriver.previous"
# b154: copy to a new file, then rename over the old one. Copying INTO the old file keeps its inode, and every process
# that has the plugin loaded (Brave, WindowServer) then fails macOS's code-signature check: "SIGKILL (Code Signature
# Invalid)" at the next load (10-23, Brave crashed at start after the b154 install).
cp "$BIN" "$DST/Contents/MacOS/NVMTLDriver.new" && mv -f "$DST/Contents/MacOS/NVMTLDriver.new" "$DST/Contents/MacOS/NVMTLDriver" || { echo "STOP: copy failed"; exit 1; }
echo "plugin binary updated (previous one kept as /Library/GPUBundles/NVMTLDriver.previous)"
[ -f "$NEW/Contents/Info.plist" ] && cp "$NEW/Contents/Info.plist" "$DST/Contents/Info.plist"
if [ -n "$FULL" ]; then
    [ -f "$FULL/Contents/MacOS/air-runtime/libLLVM.dylib" ] || { echo "STOP: $FULL has no Contents/MacOS/air-runtime/libLLVM.dylib"; exit 1; }
    rm -rf "$DST/Contents/MacOS/air-runtime"
    ditto "$FULL/Contents/MacOS/air-runtime" "$DST/Contents/MacOS/air-runtime" && echo "air-runtime restored from $FULL"
    [ -f "$DST/Contents/Info.plist" ] || { [ -f "$FULL/Contents/Info.plist" ] && cp "$FULL/Contents/Info.plist" "$DST/Contents/Info.plist"; }
fi
chown -R root:wheel "$DST"; chmod -R go-w,a+rX "$DST"
xattr -dr com.apple.quarantine "$DST" 2>/dev/null
echo
if [ -f "$DST/Contents/MacOS/air-runtime/libLLVM.dylib" ]; then echo "OK: shader reader present ($DST/Contents/MacOS/air-runtime)"
else echo "WARNING: NO shader reader (air-runtime/libLLVM.dylib) - apps cannot compile shaders that are not cached."
     echo "         Get it from the 'Mesa ANV + Metal plugin' workflow artifact and run:"
     echo "         sudo $0 \"$NEW\" <that artifact>/out/nmx-metal/NVMTLDriver.bundle"; fi
[ -x /Library/GPUBundles/nvmtl/air-opt ] && echo "air-opt present (it needs air-runtime too)"
echo "Now arm again (sudo ./nmx-ws-arm.sh) so WindowServer loads the new plugin."
