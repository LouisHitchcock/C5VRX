#!/usr/bin/env python3
"""Verify the isolated C5VRX-4 snapshot, also called by IDF configuration."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"
INCLUDE = FIRMWARE / "include"
PROGRAMS = FIRMWARE / "programs"

def run(args):
    subprocess.run(args, cwd=ROOT, check=True)

def main():
    # Do not silently cross-compile the host regressions with the IDF compiler.
    cc = os.environ.get("C5VRX4_HOST_CC", "gcc")
    tracked = list(PROGRAMS.glob("*.bsasm")) + [INCLUDE / "cvbs_tables.h"]
    before = {p: p.read_text() for p in tracked}
    run([sys.executable, "tools/generate_phase8.py"])
    assert all(p.read_text() == content for p, content in before.items()), "stale generated program/table"
    cmake = (ROOT / "CMakeLists.txt").read_text()
    assert "C5VRX_ROOT" not in cmake and "EXTRA_COMPONENT_DIRS" not in cmake, "shared-main dependency"
    assert (ROOT / "partitions.csv").is_file() and (ROOT / "sdkconfig.base.defaults").is_file()
    video = "\n".join(p.read_text() for p in sorted((ROOT / "main").glob("video*.c")))
    video += (ROOT / "main/video_internal.h").read_text()
    semantic = video.split("int video_semantic_observe(", 1)[1].split("\n}\n", 1)[0]
    assert "cvbs_analyze_locked" in semantic and "phase5_pair_is_sync" not in semantic
    assert "afc_ticks" not in video and "afc2_ctrl_decide" in video
    assert "phy_rx_lab_try_actuator(afc_epoch.phy)" in video
    pipeline = (FIRMWARE / "pipeline.c").read_text()
    assert '"lane_mode"' in pipeline and '"force_ultra_v2"' not in pipeline
    assert "mode = C5VRX4_LANES_FINE" in pipeline, "fixed fine must stay the default lane policy"
    # Native AGC acquisition mask: per-boot latch, no pacing while masking,
    # DC recentring refused (bank 3 is the hold identity plane).
    assert "static int8_t active = -1;" in pipeline and "!c5vrx4_agc_mask_active() && !s_suspend_depth" in pipeline
    assert "!c5vrx4_agc_mask_active() &&" in (FIRMWARE / "cvbs_level_hw.c").read_text()
    assert "s_c5vrx4_mask_static_program" in video and '"agc_flag"' in pipeline
    # No-carrier idle raster: only from the control task, never during a menu
    # timeout, last stable standard persisted, '_' opt-out.
    assert "idle_raster_service(q_phase, idle_sync, idle_sync_age)" in video and "IDLE_RASTER_SYNC_Q" in video and '"idle_raster"' in pipeline
    assert "menu_was_active && !IDLE_RASTER_ACTIVE()" in video and '"last_std"' in pipeline
    # HDZero: level servo also under native AGC; masked snapshots decode Q3.
    level_task = video.split("void cvbs_level_task", 1)[1].split("\n}\n", 1)[0]
    assert "rf_native_agc_active" not in level_task
    assert "c5v4_cvbs_set_mask_decode(c5vrx4_agc_mask_active())" in video
    level_c = (FIRMWARE / "cvbs_level.c").read_text()
    assert "s->settled ? C5V4_LEVEL_SETTLED_DEADBAND_UV" in level_c and "DC_MIN_GAP_US      10000000" in video
    assert 'nvs_flag("radius_boost", false)' in pipeline
    # Radius boost: normal band constants unchanged, opt-out wired.
    dg3 = (ROOT / "main/direct_gain_v3.c").read_text()
    assert "s_band_normal = {13, 32, 65, 20, 17, 27, 53, 72, 30, 47, 65, 14}" in dg3
    assert '"radius_boost"' in pipeline and "direct_gain_v3_enable_boost(&s_direct_gain_v3" in video
    assert "phy_rx_lab_run_dfilt_probe(lab_observe_dfilt)" in video and "lab_run_dfilt();" in video
    assert "bw_skirt_stage(codes[choice], predemod_skirt_target_khz(target, widths[choice]));\n            bw_edge_stage();" in video and '"bw_skirt"' in pipeline
    # Edge profile: measured by calibration, used only by the AUTO gear, left
    # on any explicit bandwidth and before a calibration.
    assert "predemod_edge_choose(nbw, width, valid, BW_EDGE_CANDIDATES," in video and '"bw_ecode"' in pipeline
    assert '"bw_eskirt"' in pipeline and "c5vrx4_bw_edge_skirt()" in (ROOT / "main/rf.c").read_text()
    assert "if (fixed) bw_set_edge(true);" in video and "if (rf_fixed_bw_edge_active()) bw_set_edge(false);" in video
    assert "s_fixed_bw_edge = false; /* any explicit bandwidth leaves the edge profile */" in (ROOT / "main/rf.c").read_text()
    assert "phy_rx_lab_filter_set_skirt((int)skirt)" in (ROOT / "main/rf.c").read_text()
    assert "lab_run_bw20_wide();" in video and "ESP_ERROR_CHECK(rf_set_vendor_bandwidth_lab(true));" in video
    assert "sfw_run(&s_sfw, &ring, ceiling, floor, true, s_sfw_budget)" in video and "s_sfw.self_gate = true;" in video
    assert '"sync_fw", true' in pipeline and "esp_timer_start_periodic(s_v3_sentinel_timer, 200)" in video and "sync_flywheel_task, \"sync_fw\", 3072, NULL, 4," in video
    assert "sync_flywheel.c" in (ROOT / "component.cmake").read_text()
    # Line repair: opt-in, needs the flywheel, selectable in SETUP, source bound set.
    assert '"line_fix", true' in pipeline and "return s_line_fix && c5vrx4_sync_flywheel_enabled();" in pipeline
    assert "    C5VRX4_OPT_LINE_FIX, C5VRX4_OPT_EDGE_GEAR,\n};" in video.replace("\r\n", "\n")
    # Edge filter gear: opt-in (main never ran it), selectable in SETUP.
    assert '"edge_gear", false' in pipeline and "!c5vrx4_edge_gear_enabled() ||" in video
    assert "c5vrx4_line_repair_enabled() && reach > RAW_RING_BYTES ? reach - RAW_RING_BYTES : 0u" in video
    # Retained labs own the PHY exclusively even after gain sweeps are removed.
    gain_source = (ROOT / "main/video_gain.c").read_text()
    for task in ("direct_gain_v3_sentinel_task", "direct_gain_v3_observer_task"):
        body = gain_source.split(f"void {task}(void *arg)\n{{", 1)[1].split("\n}\n", 1)[0]
        active = body.split("bool active =", 1)[1].split(";", 1)[0]
        assert "!s_rssi_probe_active" in active and "!s_pre_q4_probe_active" in active
    idle_source = (ROOT / "main/video_idle.c").read_text()
    flywheel = idle_source.split("void sync_flywheel_task(void *arg)\n{", 1)[1].split("\n}\n", 1)[0]
    active = flywheel.split("bool active =", 1)[1].split(";", 1)[0]
    assert "!s_rssi_probe_active" in active and "!s_pre_q4_probe_active" in active
    # Per-gain DC pair re-held at once after every gain write and PHY restore,
    # through the shared owned-word hold; banned gains skipped; never while a
    # search or lab owns the PHY.
    rf_c = (ROOT / "main/rf.c").read_text()
    assert "if (force && s_post_gain_hook) s_post_gain_hook(gain_idx);" in rf_c
    assert "if (!s_native_agc && s_post_gain_hook) s_post_gain_hook(s_current_gain_val);" in rf_c
    assert "rf_set_post_gain_hook(dco_post_gain);" in video
    assert "!s_dco_tab.e[g].valid || s_dco_hold_banned[g] ||" in video
    assert "dco_hold_locked();" in (ROOT / "main/phy_rx_lab.c").read_text()

    cases = [
        ("arc_phy", ["main/arc_phy.c"]),
        ("demod_quality", []), ("fusion_receiver", []),
        ("menu_raster", ["main/menu_raster.c", "-lm"]),
        ("direct_gain_v3", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("rx_control_epoch", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("phase8_envelope", ["main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("cvbs_level", [f"-I{INCLUDE}", "firmware/cvbs_level.c"]),
        ("cvbs_snapshot", [f"-I{INCLUDE}"]),
        ("afc_state", []), ("afc_v2", ["-lm"]), ("afc_v2_ctrl", ["-lm"]),
        ("integration", ["-DC5VRX4_EXPERIMENT=1", f"-I{INCLUDE}", "-Itools/phy_lab_stubs", "main/direct_gain_v3.c", "main/arc_phy.c"]),
        ("c5vrx4_gate", ["-pthread", f"-I{INCLUDE}", "-Itools/phy_lab_stubs"]),
        ("predemod", [f"-I{INCLUDE}", "-lm"]),
        ("agc_witness", [f"-I{INCLUDE}"]),
        ("idle_raster", [f"-I{INCLUDE}"]),
        ("sync_flywheel", [f"-I{INCLUDE}", "-O2", "firmware/sync_flywheel.c", "-lm"]),
    ]
    # Windows hosts (MinGW): M_PI needs _USE_MATH_DEFINES under -std=c11, and
    # the gate and PHY-lab regressions map memory with POSIX mmap, so they run
    # on Linux CI only.
    posix = os.name != "nt"
    if not posix:
        cases = [c for c in cases if c[0] != "c5vrx4_gate"]
        print("NOTE: c5vrx4_gate and phy_rx_lab regressions skipped on Windows (POSIX sys/mman.h)")
    with tempfile.TemporaryDirectory(prefix="c5vrx4-verify-") as td:
        for name, extra in cases:
            target = str(Path(td) / name)
            run([cc, "-std=c11", "-D_DEFAULT_SOURCE", "-D_USE_MATH_DEFINES", "-Wall", "-Wextra", "-Werror", "-Imain", f"-I{INCLUDE}",
                 f"tools/test_{name}.c", *extra, "-o", target])
            run([target])
        for pinned in ((False, True) if posix else ()):
            target = str(Path(td) / f"phy_{pinned}")
            run([cc, "-pthread", "-std=c11", "-Wall", "-Wextra", "-Werror",
                 "-Itools/phy_lab_stubs", "-Imain", f"-I{INCLUDE}",
                 *(["-DC5VRX_PHY_RX_LAB_PINNED=1"] if pinned else []),
                 "tools/test_phy_rx_lab.c", "-o", target])
            run([target])
        target = str(Path(td) / "unwrap")
        run([cc, "-O3", "-std=c11", "tools/unwrap_oracle.c", "-o", target])
        run([target])
    for name in ("tools/test_unwrap.py", "tools/test_cvbs.py", "tools/test_agc_mask.py", "tools/test_flash_tools.py"):
        run([sys.executable, name])
    print(f"PASS: isolated C5VRX-4 integration, {len(cases) + (3 if posix else 1)} C regressions, exhaustive unwrap and source-driven DSP tests")

if __name__ == "__main__":
    main()
