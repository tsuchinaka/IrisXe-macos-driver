#!/bin/bash
# nmx-brightsync.sh (b134) - runs at every login (LaunchAgent com.nullmoth.brightsync). macOS's brightness service keeps
# its own value, which is "full" after a restart, while NMXGPU restores the panel to the saved level (NVRAM
# nmintelfb-brightness). This tells macOS the real level through DisplayServices - the same call the slider makes - so
# Control Center's slider matches the panel. Done 8 s and 30 s after login. Log: /tmp/nmx-brightsync.log
L=/tmp/nmx-brightsync.log
sync_once() {
    LV=$(nvram nmintelfb-brightness 2>/dev/null | awk '{print $2}')
    case "$LV" in %*) LV=$((16#${LV#%})) ;; "") return ;; *) LV=$(printf '%d' "'$LV") ;; esac
    [ "$LV" -ge 1 ] && [ "$LV" -le 100 ] || return
    R=$(osascript -l JavaScript <<JXA 2>&1
ObjC.import('CoreGraphics');
\$.NSBundle.bundleWithPath('/System/Library/PrivateFrameworks/DisplayServices.framework').load;
ObjC.bindFunction('DisplayServicesGetBrightness', ['int', ['unsigned int', 'float *']]);
ObjC.bindFunction('DisplayServicesSetBrightness', ['int', ['unsigned int', 'float']]);
var id = \$.CGMainDisplayID(); var r = Ref(); \$.DisplayServicesGetBrightness(id, r);
var was = r[0], want = $LV / 100.0, s = 'kept';
if (\$.CGDisplayIsBuiltin(id) && Math.abs(was - want) > 0.015) s = 'set (err ' + \$.DisplayServicesSetBrightness(id, want) + ')';
'macOS had ' + was + ', panel ' + want + ': ' + s;
JXA
)
    echo "$(date '+%F %T') $1: $R" >> $L
}
sleep 8;  sync_once "8 s"
sleep 22; sync_once "30 s"
exit 0
