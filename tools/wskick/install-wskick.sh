#!/bin/bash
# sudo ./install-wskick.sh            install the boot-time WindowServer restart fix
# sudo ./install-wskick.sh --remove  remove it
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
P=/Library/LaunchDaemons/com.nullmoth.wskick.plist; S=/usr/local/libexec/nmx-wskick.sh
if [ "${1:-}" = "--remove" ]; then launchctl bootout system "$P" 2>/dev/null; rm -f "$P" "$S"; echo removed; exit 0; fi
mkdir -p /usr/local/libexec
cp "$HERE/nmx-wskick.sh" "$S" && chmod 755 "$S" && chown root:wheel "$S"
cp "$HERE/com.nullmoth.wskick.plist" "$P" && chmod 644 "$P" && chown root:wheel "$P"
xattr -d com.apple.quarantine "$S" "$P" 2>/dev/null
echo "installed: $P -> $S (log: /var/log/nmx-wskick.log)"
