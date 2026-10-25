#!/bin/bash
# ci-build-intel.sh [workdir]: fetches drm-kmod and the FreeBSD LinuxKPI sources, then builds NMXGPU.kext for an Intel
# (i915) GPU, end to end, and collects every log. Meant for a GitHub Actions macOS runner, but runs the same on any Mac
# with Xcode. It never stops at the first failing stage: each stage's result and its top errors go to the summary so one
# run shows everything that is broken.
#
#   NMX_MIN_OS     macOS deployment target for the kext objects and the link         (default 14.0, Sonoma)
#   NMX_NO_XE      1 = leave the xe driver out (Tiger Lake and older are i915)       (default 1)
#   DRM_KMOD_REF   drm-kmod branch, tag or commit                                    (default: pinned known-good commit)
#   FREEBSD_REF    freebsd-src branch, tag or commit                                 (default: pinned known-good commit)
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
DRM_KMOD_REF=${DRM_KMOD_REF:-c16de0f9fa84f70b9551f69fde837a7f3a73c0e9}   # pinned: last known-good build (was master)
FREEBSD_REF=${FREEBSD_REF:-b42793d4bcdf73475b77a26c9dbacd45f8876621}   # pinned: last known-good build (was main; upstream LinuxKPI drifts)
CI=$W/ci
mkdir -p "$CI" "$W/out" "$NMX_K"
SUM=$CI/summary.md
: > "$SUM"; : > "$CI/status.txt"

say() { echo "$*" | tee -a "$SUM"; }
group() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::group::$1"; return 0; }
endgroup() { [ -n "${GITHUB_ACTIONS:-}" ] && echo "::endgroup::"; return 0; }

# fetch <url> <ref> <dir> [sparse path...]: shallow fetch of one ref (branch, tag or commit); sparse paths limit the checkout
# b85: a GitHub network hiccup left drm-kmod unfetched on one runner (10-19, "MISSING drm-kmod/i915/Makefile") - 3 tries
fetch() {
    for attempt in 1 2 3; do
        fetch_once "$@" && return 0
        echo "fetch $1 failed (attempt $attempt) - retrying in 20 s" >&2; sleep 20
    done
    return 1
}
fetch_once() {
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
    FB=$(fetch https://github.com/freebsd/freebsd-src.git "$FREEBSD_REF" "$NMX_K/freebsd-src" sys/compat/linuxkpi sys/contrib/ck sys/contrib/dev/acpica/include) \
        || { say "- **FAILED** fetching freebsd-src ($FREEBSD_REF)"; FB=""; }
    say "- drm-kmod $DRM_KMOD_REF = \`${DK:-unknown}\`"
    say "- freebsd-src $FREEBSD_REF = \`${FB:-unknown}\` (sys/compat/linuxkpi, sys/contrib/ck, sys/contrib/dev/acpica/include)"
else
    say "- sources reused from $NMX_K"
fi
endgroup
for need in drm-kmod/kconfig.mk drm-kmod/amd/amdgpu/Makefile drm-kmod/i915/Makefile \
            freebsd-src/sys/compat/linuxkpi/common/include freebsd-src/sys/contrib/ck/src/ck_epoch.c \
            freebsd-src/sys/contrib/dev/acpica/include/acpi.h; do
    [ -e "$NMX_K/$need" ] || { say "- **MISSING** \`$need\`"; MISSING=1; }
done
if [ -n "${MISSING:-}" ]; then
    say ""; say "Stopping: the source trees are incomplete."; exit 1
fi

# ---- working copy of the port (the scripts write objects and logs next to themselves) --------------------------------
rm -rf "$NMX_X"; mkdir -p "$NMX_X"
cp -R "$HERE/." "$NMX_X/" || { say "**could not copy the port to $NMX_X**"; exit 1; }
cd "$NMX_X" || exit 1
# a zip or web upload of the repo drops the executable bit; the stage scripts call each other as ./name.sh
chmod +x ./*.sh kext/*.sh kext-npu/*.sh 2>/dev/null
mkdir -p obj obj-i915 lkobj lklogs srcobj xnuobj logs logs-i915 fbroot
# fbroot is an include root: FreeBSD sources write <compat/linuxkpi/...> and <contrib/...>, relative to sys/
ln -sfn "$NMX_K/freebsd-src/sys/compat" fbroot/compat
ln -sfn "$NMX_K/freebsd-src/sys/contrib" fbroot/contrib

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
# b80: an IOKit-side file that does not compile must not end as a kext with missing pieces (kmutil refuses it later)
if grep -q "^runtime rc=0$" "$CI/status.txt"; then
    run link ./kext/build.sh "$W/out"
else
    say ""; say "**runtime stage failed (see ci/runtime.txt: XNU FAIL) - NMXGPU.kext NOT linked**"
    echo "link rc=skipped" >> "$CI/status.txt"
fi

# ---- NMIntelFB.kext: small framebuffer that re-publishes the screen the firmware lit (no i915 needed) -----------------
build_intelfb() {
    S="$HERE/../../kexts/NMIntelFB"; O="$W/out/NMIntelFB.kext"
    [ -f "$S/intelfb.cpp" ] || { echo "no $S/intelfb.cpp"; return 1; }
    mkdir -p "$O/Contents/MacOS" "$W/ifb" || return 1
    SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$S" \
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin \
        -fno-common -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
        -c "$S/intelfb.cpp" -o "$W/ifb/intelfb.o" || return 1
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -nostdlib -Xlinker -kext \
        -lkmodc++ -lkmod -lcc_kext "$W/ifb/intelfb.o" -o "$O/Contents/MacOS/NMIntelFB" || return 1
    cp "$S/Info.plist" "$O/Contents/Info.plist"
}
run intelfb build_intelfb
# ---- NVRMAGDC.kext: the AppleGraphicsDeviceControl object WindowServer's IOPresentment needs before it sends frames to
# the display pipe (b84: it answers for NMXGPUFB when there is no NVIDIA framebuffer) --------------------------------
build_agdc() {
    S="$HERE/../../kexts/NVRMAGDC"; O="$W/out/NVRMAGDC.kext"
    mkdir -p "$O/Contents/MacOS" "$W/agdc" || return 1
    SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$S" \
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -std=c++17 -fno-rtti -fno-exceptions -fno-builtin \
        -fno-common -Wall -Wno-unused-parameter -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
        -c "$S/nvrm-agdc.cpp" -o "$W/agdc/agdc.o" || return 1
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -nostdlib -Xlinker -kext \
        -lkmodc++ -lkmod -lcc_kext "$W/agdc/agdc.o" -o "$O/Contents/MacOS/NVRMAGDC" || return 1
    cp "$S/Info.plist" "$O/Contents/Info.plist"
    echo "NVRMAGDC.kext: $(stat -f %z "$O/Contents/MacOS/NVRMAGDC") bytes"
}
run agdc build_agdc
# drmprobe: user-space check of the i915 render node (tools/drmprobe), the first acceleration step
build_drmprobe() {
    xcrun clang -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -O2 -Wall \
        "$HERE/../../tools/drmprobe/drmprobe.c" -o "$W/out/drmprobe"
    xcrun clang -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -O2 -Wall \
        "$HERE/../../tools/gpuexec/gpuexec.c" -o "$W/out/gpuexec"
    xcrun clang -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -O2 -Wall \
        "$HERE/../../tools/gpuexec/gpustress.c" -o "$W/out/gpustress"
}
run drmprobe build_drmprobe

# ---- NMXAccel.kext: the IOAccelerator for the GPU NMXGPU drives (kexts/NVRM/accel built with -DNMX_ACCEL) -----------
# It shares its source with the NVIDIA accelerator, which includes NVIDIA's open-gpu-kernel-modules headers (types and
# the nvkms kapi); the RM build's own -D list is not needed for the NMX_ACCEL paths.
build_nmxaccel() {
    local S="$HERE/../../kexts/NVRM" O="$W/out/NMXAccel.kext" OG="$W/ogkm"
    if [ ! -d "$OG/src" ]; then
        git clone -q --depth 1 --branch 610.57.04 https://github.com/NVIDIA/open-gpu-kernel-modules.git "$OG" \
          || git clone -q --depth 1 https://github.com/NVIDIA/open-gpu-kernel-modules.git "$OG" || return 1
    fi
    echo "open-gpu-kernel-modules: $(git -C "$OG" describe --tags --always 2>/dev/null)"
    local SDK KHDR NV KMS; SDK=$(xcrun --show-sdk-path); KHDR="$SDK/System/Library/Frameworks/Kernel.framework/Headers"
    NV="$OG/src/nvidia"; KMS="$OG/src/nvidia-modeset"
    local INC=( -I"$NV/arch/nvalloc/unix/include" -I"$NV/arch/nvalloc/common/inc" -I"$OG/src/common/sdk/nvidia/inc"
      -I"$OG/src/common/sdk/nvidia/inc/hw" -I"$OG/src/common/inc" -I"$OG/src/common/shared/inc" -I"$NV/inc" -I"$NV/inc/os"
      -I"$NV/inc/kernel" -I"$NV/kernel/inc" -I"$NV/interface" -I"$NV/inc/libraries" -I"$NV/generated"
      -I"$KMS/os-interface/include" -I"$KMS/kapi/interface" -I"$KMS/kapi/include" -I"$OG/src/common/unix/nvidia-push/interface"
      -I"$KMS/interface" -I"$KMS/include" -I"$OG/src/common/unix/common/inc" -I"$OG/src/common/modeset"
      -I"$OG/src/common/unix/common/utils/interface" )
    mkdir -p "$O/Contents/MacOS" "$W/accel"
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -mkernel -nostdinc -I"$KHDR" -I"$S" "${INC[@]}" \
        -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -DNVCPU_X86_64=1 -DNV_UNIX=1 -DNMX_ACCEL \
        -std=c++17 -fno-rtti -fno-exceptions -fno-builtin -fno-common -Wall -Wno-unused-parameter \
        -Wno-inconsistent-missing-override -Wno-unused-function -O2 \
        -I"$S/accel" -I"$S/accel/iofam" -I"$S/accel/re" -c "$S/accel/nvrm-accel.cpp" -o "$W/accel/nmx-accel.o" || return 1
    xcrun clang++ -arch x86_64 -mmacosx-version-min="$NMX_MIN_OS" -fapple-kext -nostdlib -Xlinker -kext -lkmodc++ -lkmod \
        -lcc_kext "$W/accel/nmx-accel.o" -o "$O/Contents/MacOS/NMXAccel" || return 1
    cp "$S/accel/nmx/Info.plist" "$O/Contents/Info.plist"
    echo "NMXAccel.kext: $(stat -f %z "$O/Contents/MacOS/NMXAccel") bytes"
}
run nmxaccel build_nmxaccel

# ---- summary -------------------------------------------------------------------------------------------------------
say ""
say "## Stages"
say ""
for st in flags core i915 runtime link intelfb agdc drmprobe nmxaccel; do
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
for st in core i915 runtime link intelfb agdc nmxaccel; do
    if [ -s "$CI/$st.txt" ]; then
        say ""; say "### $st: last lines"; say '```'
        tail -n 45 "$CI/$st.txt" | tee -a "$SUM"
        say '```'
    fi
done

tar czf "$CI/logs.tgz" -C "$NMX_X" logs logs-i915 lklogs srclog_*.txt xlog_*.txt link.log srcs.txt srcs-i915.txt amd.flags 2>/dev/null || true
[ -n "${GITHUB_STEP_SUMMARY:-}" ] && cat "$SUM" >> "$GITHUB_STEP_SUMMARY"
# a green run must mean the kext linked
[ -f "$W/out/NMXGPU.kext/Contents/MacOS/NMXGPU" ] || exit 1
exit 0
