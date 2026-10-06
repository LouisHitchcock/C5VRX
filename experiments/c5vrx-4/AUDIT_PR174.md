# External range audit of PR #174: findings and status

Audit: "Diepe firmware- en Espressif-audit van C5VRX PR #174" (head
`9de73eb`, 2026-10-06). Each finding below lists what this PR changed and
what remains open, and why. Host evidence is not an RF measurement; every
"fixed" item still needs the board/goggle check named in its row.

| § | Finding | Status | Where |
|---|---|---|---|
| 3 | Gain controller decoded the 5 GHz table with the 2.4 GHz model (RF codes, spans, no start counters) and the minimum of three maxima | **Fixed.** Verified by disassembling the pinned `libphy.a` (SHA256 `dbf33c41…104fffb`): 5 GHz spans `10,7,8,5,8,5,4,7`, start counters `12,13,15,12…`, RF codes `24,20,0,65,97,162,354,419,487`, max at `+0x126`, generator `BB=[c/6]`, `fine=5-c%6`, last stage to counter 41 (G0..G83). `arc_phy.c` reproduces it; tests check the audit's emulated vendor tuples; `dg3_map` v2 drops maps learned on the old model | `main/arc_phy.c`, `tools/test_arc.c` |
| 3.4 | G81..G83 refused | **Fixed** (real maximum used). Not blindly "more range": same RF stage, more BB/fine gain; V5's clip/fold handling still applies | same |
| 4.1 | Digital DC recentring flag shown as enabled while the service returns at once | **Status honest**: `dc_recenter_requested`, `state=blocked_live_lut`. The correction itself stays off until a proven atomic/stopped-engine LUT path exists (live read-back was random) | `main/video.c`, `cvbs_level_hw.c` |
| 4.2 | Hardware DCO only at the table maximum, only after an idle search | **Fixed in scope**: searched per gain over the top RF stage (G54..max) in the idle raster, held per current gain, re-searched after 120 s, stored per channel in NVS so a boot with the VTX on starts from measured codes. **Open**: drift while a carrier stays on cannot be searched; vendor per-channel recalibration (ESPARGOS `rx_recalibration.c` route) not attempted without a measurement setup | `main/video.c`, `main/phy_rx_lab.c` |
| 5 | Sampling autocheck latched before its scan, needed coherence >= 80, never retried | **Fixed**: states `unverified/checking/settled/failed`, scan returns its result, latch only on a measured-good phase, retry after 10 s (max 5 scans per boot), coherence >= 60 with the envelope in band, re-check after a retune. **Open**: native AGC route not covered; the isolated-glitch metric is relative, not an absolute error rate | `main/video.c` |
| 6.1/6.2 | Span75 post-detection aliasing, ~0.5-1.3 dB vs a better-averaged path | **Open (research)**: a better average needs more TX lookups than the measured BitScrambler throughput. The grey class-3 output and wider HISTORY prior of this PR are the free part (+0.3-0.4 dB at C/N 3-5 dB, host model) | `SYNC_FLYWHEEL.md` |
| 6.3 | Chroma attenuated by 75 ns discrimination + hold (2.1 dB NTSC, 3.3 dB PAL burst) | **Open**: a per-sample LUT cannot shape frequency; needs a different output cadence/route | — |
| 7 | Edge gear needs coherence >= 30, may stay off in bad acquisition; BW calibration needs a carrier-free boot | **Open (research)**: changing the gear without acquisition data risks the old hunting; calibration state is shown (`PREDEMOD_BW calibrations/last`, menu `FIXED UNCAL`) | — |
| 8 | Level servo observes but never writes; CFO/levels unregulated | **Status honest**: `state=observing applied=0 reason=live_lut_refused`. **Open**: a stopped-engine or vertical-blank LUT update must be proven first; AFC stays operator-selected (estimator uncalibrated) | `cvbs_level_hw.c` |
| 9 | Flywheel overwrote clean pictures (re-acquisition at new phases, `rebuild_lines` on predictions), budget floor above its time target | **Fixed by redesign**: writes only inside the V5 observer's fade window and only from a stable lock, sampled maintenance tracking, phase-keeping jumps instead of re-acquisition, budget floor 128. Old stored `sync_fw=1` is safe with the new design. **Open**: predictor uses signed endpoint wraps, not the TX winding classes; runtime CPU on the board still to be measured | `sync_flywheel.c`, `main/video.c` |
| 10.1 | `phy_11p_set` modem vs analog parts | **Open (lab)**: needs a split A/B (modem-only / filter-only / both) with equal gain/DC/CFO on R3/R8/top channels; the existing `:` lab applies both | — |
| 2 | Comments/README describing the old `[D,D]` path; lanes "for every owner" | **Fixed** | `main/video.c`, `README.md` |

The audit's sensitivity procedure (calibrated attenuator, three endpoints,
many boots) is the way to turn these into a measured dB total; nothing here
claims one.
