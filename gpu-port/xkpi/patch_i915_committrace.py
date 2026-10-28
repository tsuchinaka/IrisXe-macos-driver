#!/usr/bin/env python3
"""patch_i915_committrace.py <i915 display/intel_display.c>: printk markers along intel_atomic_commit and
intel_atomic_commit_tail, so a hang inside the first mode set shows the last step reached in the log.
Idempotent; exits non-zero when an anchor is missing or not unique."""
import sys, pathlib
p = pathlib.Path(sys.argv[1])
s = p.read_text()
if "NMX_COMMIT_TRACE" in s:
    print("already patched"); sys.exit(0)

def pr(msg):
    return '\tprintk("NullMoth commit: %s\\n");\n' % msg

# (anchor line text, message before, message after); the anchor is one complete statement on one line
steps = [
    ("\tret = intel_atomic_prepare_commit(state);\n", "prepare planes (pins the framebuffer)", None),
    ("\tret = intel_atomic_setup_commit(state, nonblock);\n", "prepare done, setup commit", None),
    ("\t\t\tflush_workqueue(dev_priv->display.wq.modeset);\n", None, None),   # handled below (needs braces)
    ("\t\tintel_atomic_global_state_wait_for_dependencies(state);\n", None, None),
]
steps = [
    ("\tintel_atomic_global_state_wait_for_dependencies(state);\n", None, "tail: dependencies done"),
    ("\tintel_atomic_commit_fence_wait(state);\n", "tail: waiting for the commit fence", "tail: commit fence done"),
    ("\twakeref = intel_display_power_get(dev_priv, POWER_DOMAIN_DC_OFF);\n", "tail: taking DC_OFF power", "tail: DC_OFF power taken"),
    ("\tintel_commit_modeset_disables(state);\n", "tail: modeset disables", "tail: modeset disables done"),
    ("\t\tintel_set_cdclk_pre_plane_update(state);\n", "tail: cdclk pre-plane update", "tail: cdclk pre-plane done"),
    ("\tdev_priv->display.funcs.display->commit_modeset_enables(state);\n", "tail: modeset enables", "tail: modeset enables done"),
    ("\tintel_wait_for_vblank_workers(state);\n", "tail: waiting for vblank workers", "tail: vblank workers done"),
    ("\tdrm_atomic_helper_wait_for_flip_done(dev, &state->base);\n", "tail: waiting for flip done", "tail: flip done"),
    ("\t\tintel_atomic_dsb_wait_commit(new_crtc_state);\n", None, "tail: DSB wait done"),
    ("\tintel_dbuf_post_plane_update(state);\n", "tail: watermarks done, dbuf post-plane", "tail: dbuf post-plane done"),
    ("\t\tintel_post_plane_update(state, crtc);\n", "tail: post-plane update", "tail: post-plane update done"),
    ("\t\tintel_modeset_verify_crtc(state, crtc);\n", "tail: verifying the CRTC state", "tail: CRTC state verified"),
    ("\tintel_check_cpu_fifo_underruns(dev_priv);\n", "tail: checking FIFO underruns", "tail: CPU FIFO checked"),
    ("\tintel_check_pch_fifo_underruns(dev_priv);\n", None, "tail: PCH FIFO checked"),
    ("\t\tintel_verify_planes(state);\n", None, "tail: planes verified"),
    ("\tintel_sagv_post_plane_update(state);\n", "tail: SAGV post-plane", "tail: SAGV post-plane done"),
    ("\tdrm_atomic_helper_commit_hw_done(&state->base);\n", "tail: hw_done", "tail: hw_done signalled"),
    ("\tintel_display_power_put_async_delay(dev_priv, POWER_DOMAIN_DC_OFF, wakeref, 17);\n", "tail: releasing DC_OFF power", "tail: DC_OFF released"),
    ("\tqueue_work(dev_priv->display.wq.cleanup, &state->cleanup_work);\n", None, "tail: end"),
]
for anchor, before, after in steps:
    if s.count(anchor) != 1:
        print("ERROR: anchor not exactly once:", repr(anchor), s.count(anchor)); sys.exit(1)
    ind = anchor[:len(anchor) - len(anchor.lstrip("\t"))]
    new = ""
    if before: new += ind + 'nmx_step("commit %s");\n' % before
    new += anchor
    if after: new += ind + 'nmx_step("commit %s");\n' % after
    s = s.replace(anchor, new, 1)

# numbered commit start, and the return of a blocking commit
a = "\tret = intel_atomic_prepare_commit(state);\n"
if s.count(a) != 1:
    print("ERROR: commit begin anchor"); sys.exit(1)
s = s.replace(a, '\t{ static long nmx_commit_no; nmx_stepn(state->modeset ? "commit begin, full modeset, #" : "commit begin, no modeset, #", ++nmx_commit_no); }\n' + a, 1)
a = "\t\tintel_atomic_commit_tail(state);\n\t}\n\n\treturn 0;\n}\n"
if s.count(a) != 1:
    print("ERROR: commit return anchor"); sys.exit(1)
s = s.replace(a, "\t\tintel_atomic_commit_tail(state);\n\t\tnmx_step(\"commit: blocking commit returned\");\n\t}\n\n\treturn 0;\n}\n", 1)
# the first two commit() markers
a = "\tret = intel_atomic_prepare_commit(state);\n\tif (ret) {\n"
if s.count(a) != 1:
    print("ERROR: prepare_commit anchor"); sys.exit(1)
s = s.replace(a, '\tnmx_step("commit: prepare planes (pins the framebuffer)");\n' + a.replace("\tif (ret) {\n", "") +
              '\tif (!nmx_steps_quiet || ret) printk("NullMoth commit: prepare planes done, ret=%d\\n", ret);\n\tnmx_step("commit: prepare planes returned");\n\tif (ret) {\n', 1)
a = "\tret = intel_atomic_setup_commit(state, nonblock);\n"
if s.count(a) != 1:
    print("ERROR: setup_commit anchor"); sys.exit(1)
s = s.replace(a, a + '\tif (!nmx_steps_quiet || ret) printk("NullMoth commit: setup commit done, ret=%d\\n", ret);\n\tnmx_step("commit: setup commit returned");\n', 1)

# start of the tail
a = "\tint i;\n\n\tfor_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i)\n\t\tintel_atomic_dsb_prepare(state, crtc);\n"
if s.count(a) != 1:
    print("ERROR: tail start anchor"); sys.exit(1)
s = s.replace(a, "\tint i;\n\n\tnmx_step(\"commit tail start\");\n\tfor_each_new_intel_crtc_in_state(state, crtc, new_crtc_state, i)\n\t\tintel_atomic_dsb_prepare(state, crtc);\n", 1)

i = s.index("static void hsw_crtc_enable(struct intel_atomic_state *state,")
j = s.index("\n}\n", i)
body = s[i:j]
for call, msg in [("\tintel_encoders_pre_pll_enable(state, crtc);\n", "crtc enable: pre-PLL"),
                  ("\tintel_encoders_pre_enable(state, crtc);\n", "crtc enable: encoder pre-enable (panel power, link training)"),
                  ("\tintel_encoders_enable(state, crtc);\n", "crtc enable: encoder enable (backlight)")]:
    if body.count(call) != 1:
        print("ERROR: hsw_crtc_enable anchor", repr(call)); sys.exit(1)
    body = body.replace(call, '\tnmx_step("%s");\n' % msg + call + '\tnmx_step("%s done");\n' % msg, 1)
s = s[:i] + body + s[j:]
# 10-18 boot black screen: the readout reported pipe A "disabled" although the firmware had it lit (PIPECONF_A
# 0xc0000024, TRANS_DDI_FUNC_CTL_A 0x8a000102 just before attach), so i915 never took the firmware picture over and its
# sanitize step switched the panel's DDI off. Log what the readout actually sees for each pipe.
a = "\tenabled_transcoders = hsw_enabled_transcoders(crtc);\n"
if s.count(a) != 1:
    print("ERROR: readout anchor"); sys.exit(1)
s = s.replace(a, a + """\tif (crtc->pipe == PIPE_A)
\t\tprintk("NullMoth readout: pipe A transcoders 0x%x, TRANS_DDI_FUNC_CTL_A 0x%08x TRANSCONF_A 0x%08x, power pipe %d trans %d\\n",
\t\t       (unsigned)enabled_transcoders,
\t\t       intel_de_read(dev_priv, TRANS_DDI_FUNC_CTL(dev_priv, TRANSCODER_A)),
\t\t       intel_de_read(dev_priv, TRANSCONF(dev_priv, TRANSCODER_A)),
\t\t       intel_display_power_is_enabled(dev_priv, POWER_DOMAIN_PIPE(PIPE_A)),
\t\t       intel_display_power_is_enabled(dev_priv, POWER_DOMAIN_TRANSCODER(TRANSCODER_A)));
""", 1)

a = "\t\t\t\t\t\t       POWER_DOMAIN_PIPE(crtc->pipe)))\n\t\treturn false;\n\n\tpipe_config->shared_dpll = NULL;\n"
if s.count(a) != 1:
    print("ERROR: pipe power anchor"); sys.exit(1)
s = s.replace(a, "\t\t\t\t\t\t       POWER_DOMAIN_PIPE(crtc->pipe))) {\n\t\tif (crtc->pipe == PIPE_A)\n\t\t\tprintk(\"NullMoth readout: pipe A power domain reads as off\\n\");\n\t\treturn false;\n\t}\n\n\tpipe_config->shared_dpll = NULL;\n", 1)

# b190 (speed / heat): the two printk above ran on EVERY commit - with a 60 Hz desktop ~120 kernel log lines a second
# (10381 in 5 min, tahoe-icons-errors 10-26), each one work for logd. Quiet like the nmx_step breadcrumbs once
# WindowServer owns the screen (nmx_steps_quiet, b126); a failed commit (ret != 0) is still logged.
s = "/* NMX_COMMIT_TRACE */\nextern void nmx_step(const char *what);\nextern void nmx_stepn(const char *what, long n);\nextern volatile int nmx_steps_quiet;\n" + s
p.write_text(s)
print("patched", p)
