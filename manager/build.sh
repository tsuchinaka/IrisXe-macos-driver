#!/bin/bash
# manager/build.sh <output folder> - builds "IrisXe Manager.app" (needs Xcode / the command line tools; GitHub Actions has them).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: build.sh <output folder>}
APP="$OUT/IrisXe Manager.app"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/helpers"
xcrun swiftc -O -parse-as-library -target x86_64-apple-macos13.0 \
    -framework SwiftUI -framework AppKit \
    "$HERE/IrisXeManager.swift" -o "$APP/Contents/MacOS/IrisXeManager"
cp "$HERE/irisxe-manager.sh" "$APP/Contents/Resources/irisxe-manager.sh"
chmod 755 "$APP/Contents/Resources/irisxe-manager.sh"
# the acceleration helper and the brightness sync it installs (install-autoarm.sh looks for ../nmx-brightsync)
T="$HERE/../tools/macos-helpers"
cp -R "$T/nmx-autoarm" "$T/nmx-brightsync" "$APP/Contents/Resources/helpers/"
# app icon: manager/icons/icon-<letter>.png, the letter in manager/icons/CHOSEN (A, B or C)
L=$(tr -d ' \n' < "$HERE/icons/CHOSEN" 2>/dev/null || echo A); PNG="$HERE/icons/icon-$L.png"; [ -f "$PNG" ] || PNG="$HERE/icons/icon-A.png"
IS=$(mktemp -d)/AppIcon.iconset; mkdir -p "$IS"
for s in 16 32 128 256 512; do
    sips -z $s $s "$PNG" --out "$IS/icon_${s}x${s}.png" >/dev/null || true
    sips -z $((s*2)) $((s*2)) "$PNG" --out "$IS/icon_${s}x${s}@2x.png" >/dev/null || true
done
iconutil -c icns "$IS" -o "$APP/Contents/Resources/AppIcon.icns" && echo "icon: $L" || echo "icon: FAILED (app keeps the default icon)"
cat > "$APP/Contents/Info.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleExecutable</key><string>IrisXeManager</string>
  <key>CFBundleIdentifier</key><string>com.irisxe.manager</string>
  <key>CFBundleName</key><string>IrisXe Manager</string>
  <key>CFBundleDisplayName</key><string>IrisXe Manager</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSPrincipalClass</key><string>NSApplication</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>CFBundleIconFile</key><string>AppIcon</string>
  <key>NSAppleEventsUsageDescription</key><string>Asks for your password to install the driver.</string>
</dict>
</plist>
EOF
codesign -f -s - --deep "$APP" 2>/dev/null || true
echo "built: $APP"
