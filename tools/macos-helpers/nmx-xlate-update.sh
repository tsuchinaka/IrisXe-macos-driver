#!/bin/bash
# nmx-xlate-update.sh - install a new shader translator (libnvmtl_translate.dylib) from the nmxgpu-intel-build
# artifact (out/nvmtl/libnvmtl_translate.dylib).
#
#   sudo ./nmx-xlate-update.sh <path to the new libnvmtl_translate.dylib>
#
# The plugin loads the translator from NVMTLDriver.bundle/Contents/MacOS/ when one is there, else from
# /Library/GPUBundles/nvmtl/. Both places are updated when they hold a copy. Each file is copied to a NEW file and
# renamed over the old one (copying into a loaded file breaks macOS's code-signature check). The shader caches need no
# clearing: they are keyed by the translator's own hash, so every shader is translated again once.
# Apps load the new translator when they start; WindowServer at the next log out / log in or restart.
set -u
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0 $*"; exit 1; }
NEW=${1:?usage: sudo $0 <new libnvmtl_translate.dylib>}
[ -f "$NEW" ] || { echo "STOP: no $NEW"; exit 1; }
file "$NEW" | grep -q "Mach-O" || { echo "STOP: $NEW is not a Mach-O library"; exit 1; }
# b156: this Mac is Intel - an arm64 translator does not load and WindowServer restarts in a loop at login (b155).
# Checked with `file` and `grep` only: lipo and nm need the Xcode command line tools, which this Mac does not have.
ARCH=$(file -b "$NEW")
echo "$ARCH" | grep -q "x86_64" || { echo "STOP: $NEW is not built for x86_64 ($ARCH) - not installed"; exit 1; }
echo "$ARCH" | grep -q "shared library" || { echo "STOP: $NEW is not a shared library ($ARCH) - not installed"; exit 1; }
# and it must contain the entry point the plugin calls
grep -aq "_nvmtl_translate" "$NEW" || { echo "STOP: $NEW has no nvmtl_translate entry point - not installed"; exit 1; }
done_any=0
for D in /Library/GPUBundles/nvmtl /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS; do
    T="$D/libnvmtl_translate.dylib"
    if [ -f "$T" ] || [ "$D" = /Library/GPUBundles/nvmtl ]; then
        mkdir -p "$D"
        [ -f "$T" ] && cp -p "$T" "$D/libnvmtl_translate.previous.dylib"
        cp "$NEW" "$T.new" && mv -f "$T.new" "$T" || { echo "STOP: copy to $T failed"; exit 1; }
        chown root:wheel "$T"; chmod 755 "$T"; xattr -d com.apple.quarantine "$T" 2>/dev/null
        echo "updated $T (previous kept as libnvmtl_translate.previous.dylib)"; done_any=1
    fi
done
[ $done_any = 1 ] || echo "WARNING: nothing updated"
echo "Quit and reopen apps (Brave) to use it; log out and in (or restart) for WindowServer."
