#!/bin/bash
# build-metal.sh <work dir> - after tools/vk/build-anv.sh: builds the shader translator (Rust), the Metal plugin
# (NVMTLDriver.bundle) and mtlprobe, and assembles <work>/out/nmx-metal with an installer.
set -u
W="${1:?work dir}"; W=$(cd "$W" && pwd)
REPO=$(cd "$(dirname "$0")/../.." && pwd)
VK="$W/out/nmx-vulkan"; OUT="$W/out/nmx-metal"; LOG="$W/logs"; mkdir -p "$OUT/nvmtl" "$LOG"
export MACOSX_DEPLOYMENT_TARGET="${NMX_MIN_OS:-14.0}"
VKH=$(brew --prefix vulkan-headers)/include
say() { echo; echo "=== $*"; }
[ -f "$VK/libvulkan_intel.dylib" ] || { echo "no ANV build in $VK"; exit 1; }

say "translator (Rust)"
command -v cargo >/dev/null || { curl -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal >/dev/null && . "$HOME/.cargo/env"; }
cargo --version
cargo build --release --manifest-path "$REPO/translator/wrapper/Cargo.toml" --target-dir "$W/xlate-target" > "$LOG/cargo.log" 2>&1 \
  || { grep -E "^error" -A6 "$LOG/cargo.log" | head -60; tail -20 "$LOG/cargo.log"; exit 1; }
cp "$W/xlate-target/release/libnvmtl_translate.dylib" "$OUT/nvmtl/"
install_name_tool -id @rpath/libnvmtl_translate.dylib "$OUT/nvmtl/libnvmtl_translate.dylib"

say "plugin (NVMTLDriver.bundle)"
B="$OUT/NVMTLDriver.bundle/Contents"; mkdir -p "$B/MacOS"
xcrun clang -arch x86_64 -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -bundle -fobjc-arc -O1 -Wno-protocol \
  -Wno-incomplete-implementation -I"$VKH" -I"$REPO/plugin" -o "$B/MacOS/NVMTLDriver" "$REPO/plugin/NVMTLDevice.m" \
  -framework Foundation -framework Metal -framework IOSurface -framework QuartzCore -framework IOKit > "$LOG/plugin.log" 2>&1 \
  || { grep -E "error:" -A3 "$LOG/plugin.log" | head -80; exit 1; }
echo "plugin warnings: $(grep -c 'warning:' "$LOG/plugin.log")"
cp "$REPO/tools/metal/Info.plist" "$B/Info.plist"

say "AIR reader (libLLVM + air-opt)"
# Metal shaders arrive as AIR (Apple's LLVM bitcode). The plugin decodes it in memory with
# Contents/MacOS/air-runtime/libLLVM.dylib, or falls back to /Library/GPUBundles/nvmtl/air-opt (opt -S).
# Without either, every newLibrary* call ends in "translation produced nothing" (10-17).
LP="${MESA_LLVM_PREFIX:-}"
AR="$B/MacOS/air-runtime"; mkdir -p "$AR"
if [ -n "$LP" ] && [ -d "$LP/lib" ]; then
  echo "LLVM $("$LP/bin/llvm-config" --version 2>/dev/null) from $LP"
  src=$(ls "$LP"/lib/libLLVM.dylib "$LP"/lib/libLLVM-*.dylib 2>/dev/null | head -1)
  [ -n "$src" ] || { echo "no libLLVM dylib in $LP/lib"; ls "$LP/lib" | grep -i llvm | head; exit 1; }
  cp -L "$src" "$AR/libLLVM.dylib"
  realname=$(otool -D "$src" | tail -1 | sed 's|.*/||')
  [ "$realname" != "libLLVM.dylib" ] && cp -L "$src" "$AR/$realname"
  cp -L "$LP/bin/opt" "$OUT/nvmtl/air-opt"
  # copy every @rpath dependency (libc++, zstd, zlib, xml2...) from the conda env, recursively
  copy_deps() {
    for d in $(otool -L "$1" | awk 'NR>1 {print $1}' | grep '^@rpath/'); do
      b=${d#@rpath/}
      [ -f "$AR/$b" ] && continue
      if [ -f "$LP/lib/$b" ]; then cp -L "$LP/lib/$b" "$AR/$b"; copy_deps "$AR/$b"; else echo "  missing dep $b"; fi
    done
  }
  copy_deps "$AR/libLLVM.dylib"; copy_deps "$OUT/nvmtl/air-opt"
  for f in "$AR"/*.dylib; do install_name_tool -add_rpath @loader_path "$f" 2>/dev/null || true; done
  install_name_tool -add_rpath /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/air-runtime "$OUT/nvmtl/air-opt" 2>/dev/null || true
  for f in "$AR"/*.dylib "$OUT/nvmtl/air-opt"; do codesign -f -s - "$f" 2>/dev/null; done
  ls -la "$AR"; otool -L "$AR/libLLVM.dylib" | head -12
else
  echo "WARNING: MESA_LLVM_PREFIX not set - no AIR reader bundled; Metal shaders will not compile"
fi

say "mtlprobe"
xcrun clang -arch x86_64 -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -fobjc-arc -O1 "$REPO/tools/metal/mtlprobe.m" \
  -framework Foundation -framework Metal -o "$OUT/mtlprobe" || exit 1

say "mtldraw"
xcrun clang -arch x86_64 -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -fobjc-arc -O1 "$REPO/tools/metal/mtldraw.m" \
  -framework Foundation -framework Metal -framework CoreGraphics -framework ImageIO -o "$OUT/mtldraw" || exit 1

say "mtlwindow"
xcrun clang -arch x86_64 -mmacosx-version-min="$MACOSX_DEPLOYMENT_TARGET" -fobjc-arc -O1 "$REPO/tools/metal/mtlwindow.m" \
  -framework Foundation -framework AppKit -framework Metal -framework QuartzCore -o "$OUT/mtlwindow" || exit 1

say "assemble"
for f in libvulkan_intel.dylib libdrm.2.dylib intel_icd.json; do cp "$VK/$f" "$OUT/nvmtl/"; done
for f in "$VK"/*.dylib; do b=$(basename "$f"); [ -f "$OUT/nvmtl/$b" ] || cp "$f" "$OUT/nvmtl/"; done   # deps copied beside ANV
cp "$VK/libvulkan.1.dylib" "$OUT/nvmtl/libvulkan.dylib" 2>/dev/null || true
rm -f "$OUT/nvmtl/nmxmmap.dylib"     # not needed: ANV maps through NMXGPU itself now
cp "$REPO/tools/metal/install-metal.sh" "$OUT/" && chmod +x "$OUT/install-metal.sh" "$OUT/mtlprobe" "$OUT/mtldraw" "$OUT/mtlwindow"
for f in "$OUT/nvmtl"/*.dylib "$B/MacOS/NVMTLDriver" "$OUT/mtlprobe" "$OUT/mtldraw" "$OUT/mtlwindow"; do codesign -f -s - "$f" 2>/dev/null; done
otool -L "$OUT/nvmtl/libvulkan_intel.dylib" | tee "$LOG/anv-deps-metal.txt"
nm -gU "$OUT/nvmtl/libvulkan_intel.dylib" 2>/dev/null | grep -c nmx_drm_mmap | sed 's/^/nmx_drm_mmap symbols in ANV: /'
find "$OUT" -type f | sed "s|$OUT/||"
