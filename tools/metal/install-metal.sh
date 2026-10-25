#!/bin/bash
# install-metal.sh - installs the Metal plugin for the Iris Xe into /Library/GPUBundles:
#   NVMTLDriver.bundle (the Metal device), nvmtl/ (Mesa ANV, libdrm, the shader translator), nvmtl-allow.txt.
# Only processes named in nvmtl-allow.txt (and mtlprobe) get the device, so the rest of macOS is unaffected.
#   sudo ./install-metal.sh            sudo ./install-metal.sh --remove
set -u
GB=/Library/GPUBundles; HERE=$(cd "$(dirname "$0")" && pwd)
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
if [ "${1:-}" = "--remove" ]; then rm -rf "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt"; echo "removed"; exit 0; fi
mkdir -p "$GB"
rm -rf "$GB/NVMTLDriver.bundle" "$GB/nvmtl"
ditto "$HERE/NVMTLDriver.bundle" "$GB/NVMTLDriver.bundle" && ditto "$HERE/nvmtl" "$GB/nvmtl" || { echo "copy failed"; exit 1; }
[ -f "$GB/nvmtl-allow.txt" ] || printf '# processes that get the Intel Metal device (one name per line)\nmtlprobe\n' > "$GB/nvmtl-allow.txt"
xattr -dr com.apple.quarantine "$GB/NVMTLDriver.bundle" "$GB/nvmtl" 2>/dev/null
chown -R root:wheel "$GB/NVMTLDriver.bundle" "$GB/nvmtl" "$GB/nvmtl-allow.txt"
chmod -R a+rX "$GB/NVMTLDriver.bundle" "$GB/nvmtl" && chmod 644 "$GB/nvmtl-allow.txt"
echo "installed into $GB:"; ls -la "$GB" "$GB/nvmtl"
