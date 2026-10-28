/*
 * Keep the GT awake (experiment, boot-arg nmxgtawake=<n>). 10-17: a 60 fps load froze the whole Mac after a few thousand
 * frames, while the same jobs sent back to back never did. Between paced frames the GT goes idle: i915 parks it (wakeref
 * dropped, RPS/RC6 parked, runtime PM put) and unparks it on the next submission - dozens of times a second. These levels
 * take that cycle away one piece at a time so the freeze can be pinned on it:
 *   1  hold a GT power reference: the GT is never parked (no park/unpark work, no runtime-PM cycling)  -> still froze
 *   2  1 + hold forcewake on every domain: the hardware cannot enter RC6 either                          -> clean
 * 10-18 (build 60), after nmxgtawake=0 froze again at 58 s with the runtime-PM fix in: narrow it down further. These
 * leave parking alone (no GT power reference) and change one thing each:
 *   3  RC6 and power gating switched off in the hardware (intel_rc6_disable), no forcewake held
 *   4  forcewake held on the render domain only
 *   5  forcewake held on the GT (common) domain only
 *   6  forcewake held on every media domain only
 * Returns the level applied, or a negative value.
 */
#include "i915_drv.h"
#include "gt/intel_gt.h"
#include "gt/intel_gt_pm.h"
#include "gt/intel_rc6.h"
#include "intel_uncore.h"

int nmx_i915_keep_awake(struct drm_device *drm, int level)
{
    if (!drm || level <= 0)
        return 0;
    struct drm_i915_private *i915 = to_i915(drm);
    struct intel_gt *gt = to_gt(i915);
    if (!gt)
        return -1;
    switch (level) {
    case 1:
        intel_gt_pm_get_untracked(gt);
        return 1;
    case 2:
        intel_gt_pm_get_untracked(gt);
        intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL);
        return 2;
    case 3:
        intel_rc6_disable(&gt->rc6);
        return gt->rc6.enabled ? -3 : 3;
    case 4:
        intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_RENDER);
        return 4;
    case 5:
        intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_GT);
        return 5;
    case 6:
        intel_uncore_forcewake_get(gt->uncore, FORCEWAKE_ALL & ~(FORCEWAKE_RENDER | FORCEWAKE_GT));
        return 6;
    default:
        return -9;
    }
}

/* b79: undo nmx_i915_keep_awake() before shutdown/sleep. With the GT power reference held (levels 1-2, default 2 since
   10-18) i915's shutdown waits for the GT to go idle - which it never does - until macOS gives up: 20 s of black screen
   on every Restart / Shut Down (user report 10-19). Returns 0 when something was released. */
int nmx_i915_keep_awake_release(struct drm_device *drm, int level)
{
    if (!drm || level <= 0)
        return -1;
    struct drm_i915_private *i915 = to_i915(drm);
    struct intel_gt *gt = to_gt(i915);
    if (!gt)
        return -1;
    switch (level) {
    case 1:
        intel_gt_pm_put_untracked(gt);
        return 0;
    case 2:
        intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL);
        intel_gt_pm_put_untracked(gt);
        return 0;
    case 4:
        intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_RENDER);
        return 0;
    case 5:
        intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_GT);
        return 0;
    case 6:
        intel_uncore_forcewake_put(gt->uncore, FORCEWAKE_ALL & ~(FORCEWAKE_RENDER | FORCEWAKE_GT));
        return 0;
    default:
        return -1;   /* 3 (RC6 off) holds no reference */
    }
}

/*
 * b101: a floor under the GPU clock (boot-arg nmxgpumhz=<MHz>, 0 = leave i915's own). Build 100 measured the Tiger Lake
 * GT at 100 MHz - its lowest step - all the time, busy or not (10-20: "GPU clock min 100 avg 100 max 100 MHz", the part
 * allows 100..1300). i915 raises the clock from RPS "up" interrupts, which do not arrive in this port, so every frame
 * WindowServer drew ran 13x slower than it could: the small lags while typing and scrolling. This raises the lowest
 * step i915 may pick (the same as writing gt_min_freq_mhz in Linux sysfs). Returns the floor set in MHz, or < 0.
 */
#include "gt/intel_rps.h"

int nmx_i915_gpu_floor(struct drm_device *drm, int mhz, int *lo, int *hi)
{
    if (!drm || mhz <= 0)
        return 0;
    struct drm_i915_private *i915 = to_i915(drm);
    struct intel_gt *gt = to_gt(i915);
    if (!gt)
        return -1;
    struct intel_rps *rps = &gt->rps;
    const int hw_lo = intel_gpu_freq(rps, rps->min_freq);
    const int hw_hi = (int)intel_rps_get_max_frequency(rps);
    if (lo) *lo = hw_lo;
    if (hi) *hi = hw_hi;
    if (mhz < hw_lo) mhz = hw_lo;
    if (mhz > hw_hi) mhz = hw_hi;
    int err = intel_rps_set_min_frequency(rps, (u32)mhz);
    return err ? (err < 0 ? err : -err) : mhz;
}

/*
 * b138: GPU clock governor. i915 changes the clock from RPS up/down interrupts, which do not arrive in this port (b101),
 * so b101 pinned a fixed 800 MHz floor - fast, but the GPU then ran at 800 MHz even with nothing on screen moving, and the
 * laptop ran warm (user, 10-22). The kext now calls this every 200 ms: the busiest engine's utilisation since the last
 * call (i915's own per-engine busy time) decides between
 *   busy: floor busy_mhz, ceiling the part's maximum (as b101)     - as soon as one tick is >= 15 % busy
 *   idle: floor = ceiling = idle_mhz                              - after 5 quiet ticks (1 s) in a row
 * Returns the state (1 busy, 0 idle, -1 not changed yet) and the utilisation in *util.
 */
#include "gt/intel_engine.h"

int nmx_i915_gpu_gov_tick(struct drm_device *drm, int busy_mhz, int idle_mhz, int *util, int *actual_mhz)
{
    static ktime_t prev_busy[I915_NUM_ENGINES], prev_t[I915_NUM_ENGINES];
    static int state = -1, quiet;
    if (!drm || busy_mhz <= 0 || idle_mhz <= 0)
        return -2;
    struct drm_i915_private *i915 = to_i915(drm);
    struct intel_gt *gt = to_gt(i915);
    if (!gt)
        return -2;
    struct intel_rps *rps = &gt->rps;
    struct intel_engine_cs *engine;
    enum intel_engine_id id;
    int top = 0;
    for_each_engine(engine, gt, id) {
        ktime_t now = 0;
        ktime_t busy = intel_engine_get_busy_time(engine, &now);
        if (prev_t[id] && now > prev_t[id]) {
            s64 dt = ktime_to_ns(ktime_sub(now, prev_t[id])), db = ktime_to_ns(ktime_sub(busy, prev_busy[id]));
            int pct = dt > 0 ? (int)div64_s64(db * 100, dt) : 0;
            if (pct > top) top = pct;
        }
        prev_busy[id] = busy;
        prev_t[id] = now;
    }
    if (util) *util = top;
    /* b142: the part's real top clock (RP0). intel_rps_get_max_frequency() is the CURRENT ceiling - after the first idle
       step that was the idle clock itself, so "busy" was clamped down to it and the GPU never left 350-450 MHz (10-23). */
    const int hw_hi = intel_gpu_freq(rps, rps->max_freq);
    if (busy_mhz > hw_hi) busy_mhz = hw_hi;
    if (idle_mhz > busy_mhz) idle_mhz = busy_mhz;
    if (top >= 15) {
        quiet = 0;
        if (state != 1) {
            intel_rps_set_max_frequency(rps, (u32)hw_hi);
            intel_rps_set_min_frequency(rps, (u32)busy_mhz);
            state = 1;
        }
    } else if (++quiet >= 5 && state != 0) {
        intel_rps_set_min_frequency(rps, (u32)idle_mhz);
        intel_rps_set_max_frequency(rps, (u32)idle_mhz);
        state = 0;
    }
    if (actual_mhz) *actual_mhz = intel_gpu_freq(rps, READ_ONCE(rps->cur_freq));   /* b141: the requested clock (the
                                                                                    actual-frequency register reads 0 here) */
    return state;
}

/*
 * b152: Windows-like stepped governor (policy in the kext, xnu/nmx_kext.cpp). Two helpers:
 *   nmx_i915_gpu_util - the busiest engine's utilisation (%) since the previous call (i915's per-engine busy time)
 *   nmx_i915_gpu_pin  - pin the clock to one step: floor = ceiling = mhz (inside the part's RP1..RP0 range); returns
 *                       the clock i915 now requests
 * Same i915 calls as Linux's gt_min_freq_mhz / gt_max_freq_mhz sysfs files.
 */
int nmx_i915_gpu_util(struct drm_device *drm)
{
    static ktime_t pb[I915_NUM_ENGINES], pt[I915_NUM_ENGINES];
    if (!drm)
        return -1;
    struct intel_gt *gt = to_gt(to_i915(drm));
    if (!gt)
        return -1;
    struct intel_engine_cs *engine;
    enum intel_engine_id id;
    int top = 0;
    for_each_engine(engine, gt, id) {
        ktime_t now = 0;
        ktime_t busy = intel_engine_get_busy_time(engine, &now);
        if (pt[id] && now > pt[id]) {
            s64 dt = ktime_to_ns(ktime_sub(now, pt[id])), db = ktime_to_ns(ktime_sub(busy, pb[id]));
            int pct = dt > 0 ? (int)div64_s64(db * 100, dt) : 0;
            if (pct > top) top = pct;
        }
        pb[id] = busy;
        pt[id] = now;
    }
    return top > 100 ? 100 : top;
}

int nmx_i915_gpu_pin(struct drm_device *drm, int mhz)
{
    if (!drm || mhz <= 0)
        return -1;
    struct intel_gt *gt = to_gt(to_i915(drm));
    if (!gt)
        return -1;
    struct intel_rps *rps = &gt->rps;
    const int lo = intel_gpu_freq(rps, rps->min_freq), hi = intel_gpu_freq(rps, rps->max_freq);
    if (mhz < lo) mhz = lo;
    if (mhz > hi) mhz = hi;
    const int cur_max = (int)intel_rps_get_max_frequency(rps);
    if (mhz > cur_max) {                 /* going up: raise the ceiling first */
        intel_rps_set_max_frequency(rps, (u32)mhz);
        intel_rps_set_min_frequency(rps, (u32)mhz);
    } else {                             /* going down: lower the floor first */
        intel_rps_set_min_frequency(rps, (u32)mhz);
        intel_rps_set_max_frequency(rps, (u32)mhz);
    }
    return intel_gpu_freq(rps, READ_ONCE(rps->cur_freq));
}

/*
 * b170: panel self refresh (PSR) at run time. The build keeps enable_psr=0 (all-i915.sh): while macOS drew the screen
 * with the CPU, i915's frontbuffer tracking never saw the writes and a self-refreshing panel froze on an old picture.
 * Accelerated, every frame reaches the panel by an atomic flip, which PSR follows. The bridge turns PSR1 on once frames
 * have been flipping for a while and off again when presentation goes back to CPU-drawn frames (flip home).
 * on: 1 = PSR1 forced on (I915_PSR_DEBUG_FORCE_PSR1), 0 = off (I915_PSR_DEBUG_DISABLE).
 * Returns the number of panels switched, or a negative error.
 */
#include "display/intel_display_types.h"
#include "display/intel_display.h"
#include "display/intel_psr.h"

int nmx_i915_psr(struct drm_device *drm, int on)
{
    struct intel_encoder *encoder;
    int n = 0, err = 0;
    if (!drm)
        return -EINVAL;
    for_each_intel_encoder_with_psr(drm, encoder) {
        struct intel_dp *intel_dp = enc_to_intel_dp(encoder);
        if (!intel_dp->psr.sink_support)
            continue;
        int r = intel_psr_debug_set(intel_dp, on ? I915_PSR_DEBUG_FORCE_PSR1 : I915_PSR_DEBUG_DISABLE);
        if (r)
            err = r;
        else
            n++;
    }
    return err ? err : n;
}

/*
 * b181: external displays are found by polling. The 10-25 TV test: plugging HDMI in after boot produced no hot-plug
 * event at all (no HPD interrupt reaches i915 on this port), so the TV stayed dark. i915's own fallback for that case
 * (used when the display is runtime-suspended) switches every HPD connector to polled detection: DRM's output poll
 * worker then checks them every 10 s and sends the normal hot-plug event on a change - which the display bridge handles.
 */
#include "display/intel_hotplug.h"

void nmx_i915_hpd_poll(struct drm_device *drm)
{
    if (drm)
        intel_hpd_poll_enable(to_i915(drm));
}
