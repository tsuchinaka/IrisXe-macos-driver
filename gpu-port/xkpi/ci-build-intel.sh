#!/bin/bash
# ci-build-intel.sh [workdir]: fetches drm-kmod and the FreeBSD LinuxKPI sources, then builds NMXGPU.kext for an Intel
# (i915) GPU, end to end, and collects every log. Meant for a GitHub Actions macOS runner, but runs the same on any Mac
# with Xcode. It never stops at the first failing stage: each stage's result and its top errors go to the summary so one
# run shows everything that is broken.
#
#   NMX_MIN_OS     macOS deployment target for the kext objects and the link         (default 14.0, Sonoma)
#   NMX_NO_XE      1 = leave the xe driver out (Tiger Lake and older are i915)       (default 1)
#   DRM_KMOD_REF   drm-kmod branch, tag or commit                                    (default master)
#   FREEBSD_REF    freebsd-src branch, tag or commit                                 (default main)
#   NMX_SKIP_FETCH 1 = use the sources already under <workdir>/kpi                   (default unset)
#
# Output under <workdir>: out/NMXGPU.kext (if the link worked), ci/summary.md, ci/*.txt, ci/logs.tgz
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
W=${1:-$PWD/nmx-build}
mkdir -p "$W" && W=$(cd "$W" && pwd)
export NMX_MIN_OS=${NMX_MIN_OS:-14.0}
export NMX_NO_XE=${NMX_NO_XE:-1}
export NMX_PY=${NMX_PY:-$(command -v python3)}
export NMX_K=$W/kpi
export NMX_X=$W/xkpi
DRM_KMOD_REF=${DRM_KMOD_REF:-master}
FREEBSD_REF=${FREEBSD_REF:-main}
CI=$W/ci
mkdir -p "$CI" "$W/out" "$NMX_K"
SUM=$CI/summary.md
: > "$SUM"; : > "$CI/status.txt"

say() { echo "$*" | tee -a "$SUM"; }
group() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::group::$1"; return 0; }
endgroup() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::endgroup::"; return 0; }

# fetch <url> <ref> <dir> [sparse path...]: shallow fetch of one ref (branch, tag or commit); sparse paths limit the checkout
fetch() {
    url=$1; ref=$2; dir=$3; shift 3
    rm -rf "$dir"; mkdir -p "$dir"
    ( cd "$dir" && git init -q && git remote add origin "$url" \
      && { [ $# -eq 0 ] || { git sparse-checkout init --no-cone && git sparse-checkout set "$@"; }; } \
      && git fetch -q --depth 1 --filter=blob:none origin "$ref" && git checkout -q FETCH_HEAD ) || return 1
    ( cd "$dir" && git rev-parse HEAD )
}

say "# NMXGPU (Intel i915) build"
say ""
say "- macOS target: $NMX_MIN_OS, xe: $([ -n "$NMX_NO_XE" ] && echo excluded || echo included)"
say "- Xcode: $(xcodebuild -version 2>/dev/null | tr '\n' ' ')"
say "- SDK: $(xcrun --sdk macosx --show-sdk-version 2>/dev/null)"

# ---- sources -------------------------------------------------------------------------------------------------------
group "fetch sources"
if [ -z "${NMX_SKIP_FETCH:-}" ]; then
    DK=$(fetch https://github.com/freebsd/drm-kmod.git "$DRM_KMOD_REF" "$NMX_K/drm-kmod") \
        || { say "- **FAILED** fetching drm-kmod ($DRM_KMOD_REF)"; DK=""; }
    FB=$(fetch https://github.com/freebsd/freebsd-src.git "$FREEBSD_REF" "$NMX_K/freebsd-src" sys/compat/linuxkpi sys/contrib/ck) \
        || { say "- **FAILED** fetching freebsd-src ($FREEBSD_REF)"; FB=""; }
    say "- drm-kmod $DRM_KMOD_REF = \`${DK:-unknown}\`"
    say "- freebsd-src $FREEBSD_REF = \`${FB:-unknown}\` (sys/compat/linuxkpi, sys/contrib/ck)"
else
    say "- sources reused from $NMX_K"
fi
endgroup
for need in drm-kmod/kconfig.mk drm-kmod/amd/amdgpu/Makefile drm-kmod/i915/Makefile \
            freebsd-src/sys/compat/linuxkpi/common/include freebsd-src/sys/contrib/ck/src/ck_epoch.c; do
    [ -e "$NMX_K/$need" ] || { say "- **MISSING** \`$need\`"; MISSING=1; }
done
if [ -n "${MISSING:-}" ]; then
    say ""; say "Stopping: the source trees are incomplete."; exit 1
fi

# ---- working copy of the port (the scripts write objects and logs next to themselves) --------------------------------
rm -rf "$NMX_X"; mkdir -p "$NMX_X"
cp -R "$HERE/." "$NMX_X/" || { say "**could not copy the port to $NMX_X**"; exit 1; }
cd "$NMX_X" || exit 1
mkdir -p obj obj-i915 lkobj lklogs srcobj xnuobj logs logs-i915 fbroot

# run <name> <command...>: runs a stage, keeps its output, never aborts the whole build
run() {
    name=$1; shift
    group "$name"
    "$@" 2>&1 | tee "$CI/$name.txt"
    rc=${PIPESTATUS[0]}
    endgroup
    echo "$name rc=$rc" >> "$CI/status.txt"
    return 0
}

run flags "$NMX_PY" gen_flags.py "$NMX_K/drm-kmod" amd.flags
[ -s amd.flags ] || { say ""; say "**amd.flags was not produced; see ci/flags.txt**"; }

run core ./all.sh 60
run i915 ./all-i915.sh 60
run runtime ./rt.sh
run link ./kext/build.sh "$W/out"

# ---- summary -------------------------------------------------------------------------------------------------------
say ""
say "## Stages"
say ""
for st in flags core i915 runtime link; do
    rc=$(sed -n "s/^$st rc=//p" "$CI/status.txt")
    line=$(grep -E "^(sources:|runtime sources:)" "$CI/$st.txt" 2>/dev/null | tail -1)
    say "- **$st**: exit $rc ${line:+- $line}"
done
for f in srcs.txt srcs-i915.txt; do
    [ -f "$f" ] && say "- $f lists $(wc -l < "$f" | tr -d ' ') sources"
done
if [ -f "$W/out/NMXGPU.kext/Contents/MacOS/NMXGPU" ]; then
    say ""; say "**NMXGPU linked:** $(shasum -a 256 "$W/out/NMXGPU.kext/Contents/MacOS/NMXGPU" | cut -c1-16), $(stat -f %z "$W/out/NMXGPU.kext/Contents/MacOS/NMXGPU") bytes"
    otool -l "$W/out/NMXGPU.kext/Contents/MacOS/NMXGPU" 2>/dev/null | grep -A4 LC_BUILD_VERSION | sed 's/^/    /' | tee -a "$SUM"
else
    say ""; say "**NMXGPU did not link.**"
fi
for st in core i915 runtime link; do
    if [ -s "$CI/$st.txt" ]; then
        say ""; say "### $st: last lines"; say '```'
        tail -n 45 "$CI/$st.txt" >> "$SUM"
        say '```'
    fi
done

tar czf "$CI/logs.tgz" -C "$NMX_X" logs logs-i915 lklogs srclog_*.txt xlog_*.txt link.log srcs.txt srcs-i915.txt amd.flags 2>/dev/null || true
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$SUM" >> "$GITHUB_STEP_SUMMARY"
exit 0
