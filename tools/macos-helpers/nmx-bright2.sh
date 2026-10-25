#!/bin/bash
# nmx-bright2.sh - asks macOS's own brightness API (DisplayServices, what Control Center's slider uses) what it thinks
# the brightness is, then sets it to the panel's saved level so the slider and the panel agree. No sudo.
#   ./nmx-bright2.sh          Report: /Users/Shared/nmx-bright2.txt
O=/Users/Shared/nmx-bright2.txt
LV=$(nvram nmintelfb-brightness 2>/dev/null | awk '{print $2}')
# nvram prints the byte as %XX or as a character
case "$LV" in %*) LV=$((16#${LV#%})) ;; "") LV=-1 ;; *) LV=$(printf '%d' "'$LV") ;; esac
{ echo "=== $(date)   saved panel level (NVRAM): $LV %"
  osascript -l JavaScript <<JXA 2>&1
ObjC.import('CoreGraphics');
var b = \$.NSBundle.bundleWithPath('/System/Library/PrivateFrameworks/DisplayServices.framework'); b.load;
ObjC.bindFunction('DisplayServicesGetBrightness', ['int', ['unsigned int', 'float *']]);
ObjC.bindFunction('DisplayServicesSetBrightness', ['int', ['unsigned int', 'float']]);
ObjC.bindFunction('DisplayServicesCanChangeBrightness', ['bool', ['unsigned int']]);
var id = \$.CGMainDisplayID();
var r = Ref(); var e = \$.DisplayServicesGetBrightness(id, r);
var out = 'display ' + id + ' builtin=' + \$.CGDisplayIsBuiltin(id) + ' canChange=' + \$.DisplayServicesCanChangeBrightness(id) +
          '\nDisplayServices brightness BEFORE: ' + r[0] + ' (err ' + e + ')';
var lv = $LV;
if (lv >= 1 && lv <= 100) {
  var s = \$.DisplayServicesSetBrightness(id, lv / 100.0);
  delay(1.0);
  var r2 = Ref(); var e2 = \$.DisplayServicesGetBrightness(id, r2);
  out += '\nset to ' + (lv/100.0) + ' (err ' + s + ')\nDisplayServices brightness AFTER: ' + r2[0] + ' (err ' + e2 + ')';
}
out;
JXA
  echo "--- IODisplay brightness now"
  ioreg -lw0 -r -c AppleBacklightDisplay 2>/dev/null | grep -oE '"(linear-)?brightness"=\{[^}]*\}' | head -2
  echo "--- driver messages (last 2 min)"
  log show --last 2m --style compact --predicate 'eventMessage CONTAINS "NullMoth display: backlight"' 2>/dev/null | tail -8
} > $O 2>&1
chmod 644 $O; cat $O; echo "done - look at the slider now, then send me $O"
