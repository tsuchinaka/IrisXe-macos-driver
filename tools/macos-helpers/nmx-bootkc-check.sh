#!/bin/bash
# nmx-bootkc-check.sh - which of the kexts our drivers need are in the BOOT kernel collection (the one OpenCore can add
# kexts to). OpenCore can only inject a kext whose dependencies are all in the boot collection. Changes nothing.
#   ./nmx-bootkc-check.sh   -> ~/Desktop/nmx-bootkc.txt
OUT="$HOME/Desktop/nmx-bootkc.txt"
{
echo "=== $(sw_vers -productName) $(sw_vers -productVersion) ($(uname -r)) $(date)"
B=/System/Library/KernelCollections/BootKernelExtensions.kc
S=/System/Library/KernelCollections/SystemKernelExtensions.kc
kmutil inspect -B "$B" 2>/dev/null > /tmp/nmx-bkc.txt; kmutil inspect -S "$S" 2>/dev/null > /tmp/nmx-skc.txt
echo "boot collection: $(wc -l < /tmp/nmx-bkc.txt) lines, system collection: $(wc -l < /tmp/nmx-skc.txt) lines"
for k in com.apple.iokit.IOPCIFamily com.apple.iokit.IOACPIFamily com.apple.iokit.IOGraphicsFamily \
         com.apple.iokit.IOAcceleratorFamily2 com.apple.iokit.IOSurface com.apple.driver.AppleGraphicsDevicePolicy \
         com.apple.iokit.IOAVFamily com.apple.driver.AppleGraphicsControl; do
    b=$(grep -c "$k\b" /tmp/nmx-bkc.txt); s=$(grep -c "$k\b" /tmp/nmx-skc.txt)
    printf "%-48s boot:%s  system:%s\n" "$k" "$([ "$b" -gt 0 ] && echo YES || echo no)" "$([ "$s" -gt 0 ] && echo yes || echo no)"
done
echo "--- loaded NullMoth kexts and where from"
kmutil showloaded --list-only 2>/dev/null | grep -i -E "nullmoth|nmx" 
echo "--- first lines of the boot collection list (format check)"
head -8 /tmp/nmx-bkc.txt
} | tee "$OUT"
echo "saved: $OUT"
