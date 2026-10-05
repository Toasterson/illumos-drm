#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright 2026 The illumos-drm authors
"""Writes i915_nodisplay.c: the display functions the headless i915 objects
call, as stubs for a build without i915's display; with --excluded,
i915_excluded.c: the functions of the display families the display build
leaves out (DSI, HDCP, LPE audio; display-tus.txt), see EXCLUDED below.

Usage: gen-nodisplay.py <patched linux tree> <symbols.txt> > i915_nodisplay.c
       gen-nodisplay.py --excluded <patched linux tree> > i915_excluded.c

symbols.txt lists the display symbols the headless objects import (from the
link step). Each function's prototype is taken from the display headers of
the tree; the body is the "no display" answer for its return type:
nothing for void, false for bool, NULL for pointers, -ENODEV for ioctls,
0 otherwise, with the special cases of SPECIAL below. The parent
interfaces of the display glue (i915_display_*_interface) become empty
tables.
"""
import glob
import os
import re
import sys

SPECIAL = {
    # dumb buffers: no plane limits without a display; never force paging
    # alignment of the pitch.
    "intel_dumb_fb_max_stride": "return (INT_MAX);",
    # i915_driver_probe(): a device without a display, as Linux models
    # one (intel_display_device_probe's no_display case): i915->display
    # is never NULL in Linux, and the core code outside display/ reads
    # it through macros and inlines (HAS_FPGA_DBG_UNCLAIMED in
    # intel_uncore_init_mmio() was the first, a NULL dereference at the
    # first attach on a ThinkPad X230). The empty device info answers
    # every DISPLAY_INFO/DISPLAY_RUNTIME_INFO question with "no display".
    "intel_display_device_probe": "\n\t".join([
        "static const struct intel_display_device_info no_display = { 0 };",
        "struct intel_display *display;",
        "display = kzalloc(sizeof (*display), GFP_KERNEL);",
        "if (display == NULL)",
        "\treturn (ERR_PTR(-ENOMEM));",
        "display->drm = pci_get_drvdata(pdev);",
        "display->parent = parent;",
        "DISPLAY_INFO(display) = &no_display;",
        "return (display);"]),
    # i915_driver_late_release(): frees what the probe above allocated.
    "intel_display_device_remove": "kfree(display);",
}

# Headers the SPECIAL bodies need beyond the declarations' own headers
# (struct intel_display for intel_display_device_probe).
EXTRA_INCLUDES = ["display/intel_display_core.h"]

# Display functions whose work the headless build must still do: the stub
# stays empty because the illumos glue does that work instead (the note
# says where). A display symbol not listed here and not answerable with
# "no display" must be added here or to SPECIAL, never left to the default.
ELSEWHERE = {
    "ilk_display_irq_master_disable": "patch 0006",
    "ilk_display_irq_master_enable": "patch 0006",
    "intel_display_irq_reset": "patch 0006",
    "intel_display_irq_postinstall": "patch 0006",
}


# The display build (DISPLAY=1 in build.sh) compiles every display TU but
# the families below, whose external dependencies illumos does not have
# and which the platforms illumos drives the display of (Ivy Bridge,
# display version 7; patch 0011) never reach. The display objects still
# call into them; every stub answers what the real function answers on
# Ivy Bridge, and the reason says why that answer is right there (read
# from the callers in the pinned tree). There is no default: a symbol of
# these families the display objects import must be listed here, or the
# link (-z defs) fails.
#
# name: (header, body, reason). header is relative to drivers/gpu/drm/i915
# of the tree, or <drm/...> for a LinuxKPI layer header; body "" is an
# empty void function, "table" an empty interface table.
EXCLUDED = {
    # --- DSI (MIPI DSI bus; VLV/CHV, BXT/GLK and display 11+ only) ---
    "assert_dsi_pll_enabled": (
        "display/vlv_dsi_pll.h", "",
        "intel_enable_transcoder() checks the DSI PLL on GMCH displays "
        "(VLV/CHV) driving a DSI output; Ivy Bridge has no GMCH display "
        "and no DSI output. Empty, as Linux's own inline without I915."),
    "bxt_dsi_pll_is_enabled": (
        "display/vlv_dsi_pll.h", "return (false);",
        "The BXT/GLK DSI transcoder readout (bxt_get_dsi_transcoder_state(), "
        "only for platform.geminilake/broxton). false: no DSI PLL, no DSI "
        "transcoder, as Linux's inline without I915."),
    "icl_dsi_frame_update": (
        "display/icl_dsi.h", "",
        "intel_pipe_update_end() for DISPLAY_VER >= 11 with a DSI output "
        "only."),
    "icl_dsi_init": (
        "display/icl_dsi.h", "",
        "intel_ddi_init() for a VBT port with DSI on display 11+ "
        "(assert_has_icl_dsi()); Ivy Bridge has no DDI. No connector."),
    "intel_dsi_dcs_init_backlight_funcs": (
        "display/intel_dsi_dcs_backlight.h", "return (-ENODEV);",
        "intel_backlight_init_funcs() for DRM_MODE_CONNECTOR_DSI only; no "
        "DSI connector exists. Non-zero, so a caller falls back to the "
        "platform's PWM backlight functions (0 would leave none set)."),
    "vlv_dsi_init": (
        "display/vlv_dsi.h", "",
        "intel_setup_outputs() for VLV/CHV and BXT/GLK only. No connector, "
        "as Linux's inline without I915."),
    "vlv_dsi_min_cdclk": (
        "display/vlv_dsi.h", "return (0);",
        "intel_crtc_compute_min_cdclk() takes the max() of every limit; "
        "Linux returns 0 for a CRTC without a DSI output, which every CRTC "
        "is here."),
    "mipi_dsi_compression_mode": (
        "<drm/drm_mipi_dsi.h>", "return (-ENODEV);",
        "intel_dsc_dsi_pps_write() for DSI encoders with DSC (display 11+), "
        "reached only from the DSI encoders, which are not built. illumos "
        "has no MIPI DSI bus."),
    "mipi_dsi_picture_parameter_set": (
        "<drm/drm_mipi_dsi.h>", "return (-ENODEV);",
        "As mipi_dsi_compression_mode."),
    # --- HDCP (MEI/GSC firmware interface, GPL drm_hdcp_helper) ---
    "i915_display_hdcp_interface": (
        "i915_hdcp_gsc.h", "table",
        "The GSC messaging of HDCP 2.2 (display 12+). Every caller of its "
        "members (intel_parent_hdcp_gsc_*()) is in intel_hdcp.c or "
        "intel_hdcp_gsc_message.c, which are not built: no member is ever "
        "called."),
    "is_hdcp_supported": (
        "display/intel_hdcp.h", "return (false);",
        "Linux: the runtime info's has_hdcp, which Ivy Bridge does not set. "
        "So HDMI and DP connector init never call intel_hdcp_init() or "
        "intel_dp_hdcp_init(), and no connector gets the Content "
        "Protection property or an HDCP shim."),
    "intel_hdcp_init": (
        "display/intel_hdcp.h", "return (-ENODEV);",
        "Called only if is_hdcp_supported(); the caller logs \"HDCP init "
        "failed, skipping\" (debug) and goes on without HDCP."),
    "intel_dp_hdcp_init": (
        "display/intel_dp_hdcp.h", "return (-ENODEV);",
        "As intel_hdcp_init (intel_dp_init_connector(), DP MST)."),
    "intel_hdcp_atomic_check": (
        "display/intel_hdcp.h", "",
        "Linux only rewrites conn_state->content_protection and forces a "
        "modeset when the protection state or content type changes. Both "
        "are set only through the Content Protection properties, which "
        "intel_hdcp_init() attaches and which therefore do not exist: "
        "they stay at their defaults in every state, and Linux returns "
        "without a change."),
    "intel_hdcp_enable": (
        "display/intel_hdcp.h", "",
        "Linux acts only for content_protection DESIRED (never set, see "
        "intel_hdcp_atomic_check) and then fails without an HDCP shim."),
    "intel_hdcp_disable": (
        "display/intel_hdcp.h", "return (-ENOENT);",
        "Linux returns -ENOENT for a connector without an HDCP shim (every "
        "connector here); the callers ignore the value."),
    "intel_hdcp_update_pipe": (
        "display/intel_hdcp.h", "",
        "Linux returns at once for a connector without an HDCP shim."),
    "intel_hdcp_cancel_works": (
        "display/intel_hdcp.h", "",
        "Linux returns at once for a connector without an HDCP shim."),
    "intel_hdcp_cleanup": (
        "display/intel_hdcp.h", "",
        "Linux returns at once for a connector without an HDCP shim."),
    "intel_hdcp_handle_cp_irq": (
        "display/intel_hdcp.h", "",
        "Linux returns at once for a connector without an HDCP shim."),
    "intel_hdcp_component_init": (
        "display/intel_hdcp.h", "",
        "Linux returns at once unless HDCP 2.2 is supported (has_hdcp, "
        "display 10+)."),
    "intel_hdcp_component_fini": (
        "display/intel_hdcp.h", "",
        "Linux returns at once unless component_init added the HDCP "
        "component, which it never does here."),
    # --- LPE audio (platform devices, IRQ chips; VLV/CHV only) ---
    "intel_lpe_audio_init": (
        "display/intel_lpe_audio.h", "return (-ENODEV);",
        "Linux returns -ENODEV on every platform without LPE audio (all "
        "but VLV/CHV); intel_audio_init() then registers the audio "
        "component instead. As Linux's inline without I915."),
    "intel_lpe_audio_teardown": (
        "display/intel_lpe_audio.h", "",
        "intel_audio_deinit() calls it only with an LPE platform device, "
        "which is never created."),
    "intel_lpe_audio_irq_handler": (
        "display/intel_lpe_audio.h", "",
        "Called from the VLV/CHV display interrupt handler only."),
    "intel_lpe_audio_notify": (
        "display/intel_lpe_audio.h", "",
        "Linux returns at once without LPE audio (HAS_LPE_AUDIO)."),
}

# The panic screen: i915_panic.c (the display's interface to the panic
# helpers of i915_gem_pages.c, which patch 0002 leaves out) is not built
# either; this replaces its interface table. Written out, not generated.
PANIC = """/*
 * The display's panic-screen interface (i915_panic.c, not built: the DRM
 * panic handler drm_panic is not carried, and the helpers it uses in
 * i915_gem_pages.c are left out by patch 0002).
 *
 * alloc: intel_framebuffer_init() allocates one struct intel_panic per
 * framebuffer and fails the framebuffer with -ENOMEM without it, so it
 * must succeed: an empty placeholder, which intel_fb.c frees with kfree().
 * setup and finish are reached only through the primary plane's
 * get_scanout_buffer and panic_flush hooks, which only drm_panic calls:
 * setup refuses as for an object it cannot map (-EOPNOTSUPP), finish has
 * nothing to undo.
 */
struct intel_panic {
	int ip_unused;
};

static struct intel_panic *
i915_illumos_panic_alloc(void)
{
	return (kzalloc(sizeof (struct intel_panic), GFP_KERNEL));
}

static int
i915_illumos_panic_setup(struct intel_panic *panic,
    struct drm_scanout_buffer *sb)
{
	(void) panic;
	(void) sb;
	return (-EOPNOTSUPP);
}

static void
i915_illumos_panic_finish(struct intel_panic *panic)
{
	(void) panic;
}

const struct intel_display_panic_interface i915_display_panic_interface = {
	.alloc = i915_illumos_panic_alloc,
	.setup = i915_illumos_panic_setup,
	.finish = i915_illumos_panic_finish,
};
"""


def comment(text, width=76):
    """text as a C block comment of cstyle lines."""
    words, lines, cur = text.split(), [], ""
    for w in words:
        if cur and len(cur) + 1 + len(w) > width - 3:
            lines.append(cur)
            cur = w
        else:
            cur = (cur + " " + w) if cur else w
    if cur:
        lines.append(cur)
    return ["/*"] + [" * " + l for l in lines] + [" */"]


def excluded(tree):
    i915 = os.path.join(tree, "drivers/gpu/drm/i915")
    layer = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "../../../linuxkpi/include")
    out = ["/*", " * SPDX-License-Identifier: MIT", " * Copyright 2026 The illumos-drm authors", " *",
           " * Generated by drivers/i915/illumos/gen-nodisplay.py --excluded; do not",
           " * edit.",
           " *",
           " * The functions of the display families the illumos display build",
           " * leaves out (DSI, HDCP, LPE audio: display-tus.txt) and the panic",
           " * interface of i915_panic.c. Display is enabled on Ivy Bridge only",
           " * (patch 0011), which none of these families serves; each stub",
           " * answers what the real function answers there, for the reason given",
           " * above it.",
           " */", "",
           "#include <linux/slab.h>",
           "#include <drm/drm_mipi_dsi.h>",
           "#include <drm/intel/display_parent_interface.h>", "",
           "#include \"i915_drv.h\"",
           "#include \"display/intel_display_limits.h\""]
    incs, items = set(), []
    for name, (hdr, body, why) in EXCLUDED.items():
        if hdr.startswith("<"):
            path = os.path.join(layer, hdr[1:-1])
        else:
            path = os.path.join(i915, hdr)
            incs.add(hdr)
        t = open(path, errors="replace").read()
        if body == "table":
            m = re.search(r"extern\s+const\s+struct\s+(\w+)\s+" + re.escape(name) + r"\s*;", t)
            if not m:
                sys.exit(f"gen-nodisplay: no table declaration of {name} in {hdr}")
            items.append((name, why, ("table", m.group(1))))
            continue
        m = re.search(r"(^|\n)([^\n;{}#]*?\b" + re.escape(name) + r"\s*\([^;{]*?\))\s*;", t)
        if not m:
            sys.exit(f"gen-nodisplay: no declaration of {name} in {hdr}")
        items.append((name, why, ("func", " ".join(m.group(2).split()), body)))
    out += [f"#include \"{i}\"" for i in sorted(incs)] + [""]
    for name, why, it in sorted(items):
        out += comment(why)
        if it[0] == "table":
            out += [f"const struct {it[1]}", f"    {name} = {{ 0 }};", ""]
            continue
        decl, body = it[1], it[2]
        pre = decl[: decl.index(name)].strip()
        ret = re.sub(r"\b(extern|static|inline|__must_check)\b", "", pre).strip()
        args = decl[decl.index(name) + len(name):]
        argnames = re.findall(r"(\w+)\s*(?:\[[^]]*\])?\s*(?:,|\)$)", args.strip())
        voids = "".join(f"\t(void) {a};\n" for a in argnames if a != "void")
        out += [ret] + wrap(name, args) + ["{", voids.rstrip("\n") if voids else None,
                                           f"\t{body}" if body else None, "}", ""]
    out.append(PANIC)
    sys.stdout.write("\n".join(x for x in out if x is not None))


def wrap(head, args):
    """head + args as cstyle lines of at most 80 columns, continued with 4
    spaces."""
    parts = [a.strip() for a in args.strip()[1:-1].split(",")]
    lines, cur = [], head + "("
    for i, a in enumerate(parts):
        piece = a + (", " if i < len(parts) - 1 else ")")
        if len(cur) + len(piece.rstrip()) > 80:
            lines.append(cur.rstrip())
            cur = "    "
        cur += piece
    lines.append(cur.rstrip())
    return lines


def main():
    tree, symfile = sys.argv[1], sys.argv[2]
    syms = [s.strip() for s in open(symfile) if s.strip()]
    i915 = os.path.join(tree, "drivers/gpu/drm/i915")
    hdrs = sorted(glob.glob(os.path.join(i915, "display/*.h")) + glob.glob(os.path.join(i915, "*.h")))
    text = {h: open(h, errors="replace").read() for h in hdrs}
    incs, funcs, tables = set(), [], []
    for s in syms:
        for h, t in text.items():
            m = re.search(r"extern\s+const\s+struct\s+(\w+)\s+" + re.escape(s) + r"\s*;", t)
            if m:
                incs.add(os.path.relpath(h, i915))
                tables.append((m.group(1), s))
                break
            m = re.search(r"(^|\n)([^\n;{}#]*?\b" + re.escape(s) + r"\s*\([^;{]*?\))\s*;", t)
            if m:
                incs.add(os.path.relpath(h, i915))
                funcs.append((s, " ".join(m.group(2).split())))
                break
        else:
            sys.exit(f"gen-nodisplay: no declaration of {s}")
    out = ["/*", " * SPDX-License-Identifier: MIT", " * Copyright 2026 The illumos-drm authors", " *",
           " * Generated by drivers/i915/illumos/gen-nodisplay.py; do not edit.",
           " *",
           " * The display functions the headless i915 build calls, for illumos,",
           " * where i915's display is not built (decision of R5c: headless first).",
           " * Each answers as a device without a display would: nothing happens,",
           " * false, NULL, 0, or -ENODEV for the display ioctls. The display",
           " * parent interfaces are empty tables. The display device itself",
           " * exists, with Linux's \"no display\" device info, so that code",
           " * outside display/ never sees a NULL i915->display.",
           " */", "", "#include <drm/intel/display_parent_interface.h>", "", "#include \"i915_drv.h\""]
    out += [f"#include \"{i}\"" for i in sorted(incs | set(EXTRA_INCLUDES))]
    out.append("")
    for struct, name in tables:
        line = f"const struct {struct} {name} = {{ 0 }};"
        if len(line) > 80:
            out += [f"const struct {struct}", f"    {name} = {{ 0 }};", ""]
        else:
            out += [line, ""]
    for s, decl in funcs:
        pre = decl[: decl.index(s)].strip()
        ret = re.sub(r"\b(extern|static|inline|__must_check|__printf\(\d+, *\d+\))\b", "", pre).strip()
        args = decl[decl.index(s) + len(s):]
        if s in SPECIAL:
            body = SPECIAL[s]
        elif ret == "void":
            body = ""
        elif ret.endswith("*"):
            body = "return (NULL);"
        elif ret == "bool":
            body = "return (false);"
        elif s.endswith("_ioctl"):
            body = "return (-ENODEV);"
        else:
            body = "return (0);"
        argnames = re.findall(r"(\w+)\s*(?:\[[^]]*\])?\s*(?:,|\)$)", args.strip())
        voids = "".join(f"\t(void) {a};\n" for a in argnames if a != "void")
        note = [f"/* The illumos glue does this instead ({ELSEWHERE[s]}). */"] if s in ELSEWHERE else []
        out += note + [ret] + wrap(s, args) + ["{", voids.rstrip("\n") if voids else None, f"\t{body}" if body else None, "}", ""]
    sys.stdout.write("\n".join(x for x in out if x is not None))


if len(sys.argv) == 3 and sys.argv[1] == "--excluded":
    excluded(sys.argv[2])
else:
    main()
