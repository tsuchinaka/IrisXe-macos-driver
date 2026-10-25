#!/bin/bash
# install-brightsync.sh - installs the login brightness sync (Control Center's slider follows the panel after a restart).
#   sudo ./install-brightsync.sh            remove:  sudo ./install-brightsync.sh remove
[ "$(id -u)" = 0 ] || { echo "run with: sudo $0"; exit 1; }
D=$(cd "$(dirname "$0")" && pwd)
P=/Library/LaunchAgents/com.nullmoth.brightsync.plist
if [ "$1" = remove ]; then
    rm -f "$P" /usr/local/libexec/nmx-brightsync.sh; echo "removed (takes effect at the next login)"; exit 0
fi
mkdir -p /usr/local/libexec
install -m 755 -o root -g wheel "$D/nmx-brightsync.sh" /usr/local/libexec/nmx-brightsync.sh
install -m 644 -o root -g wheel "$D/com.nullmoth.brightsync.plist" "$P"
U=$(stat -f %Su /dev/console)
[ -n "$U" ] && [ "$U" != root ] && launchctl asuser "$(id -u "$U")" launchctl load -w "$P" 2>/dev/null
echo "installed - runs at every login (log: /tmp/nmx-brightsync.log)"
