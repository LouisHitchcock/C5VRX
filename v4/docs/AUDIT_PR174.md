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
| 9 | Flywheel overwrote clean pictures (re-acquisition at new phases, `rebuild_lines` on predictions), budget floor above its time target | **Fixed by redesign**: writes only on lines its own raw-ring fade detector marks (implausible-step share; the first coherence gate opened on valid noise-free video at radius 4-5 - follow-up review, fixed with an integral raw-IQ test) and only from a stable lock, sampled maintenance tracking, phase-keeping jumps instead of re-acquisition, budget floor 128. Old stored `sync_fw=1` is safe with the new design. **Open**: predictor uses signed endpoint wraps, not the TX winding classes; runtime CPU on the board still to be measured | `sync_flywheel.c`, `main/video.c` |
| 10.1 | `phy_11p_set` modem vs analog parts | **Open (lab)**: needs a split A/B (modem-only / filter-only / both) with equal gain/DC/CFO on R3/R8/top channels; the existing `:` lab applies both | — |
| 2 | Comments/README describing the old `[D,D]` path; lanes "for every owner" | **Fixed** | `main/video.c`, `README.md` |
| follow-up | Learned gain map without context identity | **Fixed**: `dg3_map` v3 stores channel, lane policy and band; only the same context imports it | `main/direct_gain_v3.*`, `main/video.c` |
| follow-up | LP core as transport monitor / IQ observer | **Open**: useful for diagnosis (RX/TX stalls, HP lag, sampling quality independent of busy HP tasks), not for RF dB; needs the LP toolchain in this isolated build and a benchmark of LP clock and HP-memory latency first | — |

The audit's sensitivity procedure (calibrated attenuator, three endpoints,
many boots) is the way to turn these into a measured dB total; nothing here
claims one.

## Public SDR research (2026-10-07)

"Wat openbare SDR-code ons leert over meer C5VRX-range" (six pinned source
trees, against `d3af38e`).

| Item | Status | Where |
|---|---|---|
| P1 Vendor RX DC/IQ calibration at the tuned frequency (ESPARGOS esp-sdr) | **Built as lab `~`**: adapted from ESPARGOS `rx_recalibration.c` (C5, 06a5ca4, GPL-3.0, credited); re-verified on C5VRX's own PHY pin by disassembling `phy_bb_init()` (call arguments and the 0x80/0x400 flag bits); a link `--wrap` keeps the vendor's internal reference tunes on the tuned frequency (13 PHY calls go through it in the linked image). DC logged before/after; per-gain DC table rebuilt. **Open**: board comparison at R3/R8/A1, cold/warm boot | `main/rx_recal.c`, `main/video.c` |
| Fade detector misled by clean fast rotors (coherence 66-72) | Already fixed in `80cbdb5` (raw-ring implausible-step detector, integral test) | `sync_flywheel.c` |
| P1 Sampling acceptance including native AGC | **Built**: autocheck and scan run under native AGC (sync fragment within 1 s as the carrier test; no AGC pause). **Open**: many boots / retunes / temperature on the board | `main/video.c` |
| P2 Hardware gain readback | **Built**: forced index in 0x600A702C vs V5's index every 250 ms, mismatch counter, vendor tuple in `!` | `main/video.c` |
| P1 Filter labs `;` `/`, 11p split | **Open (measurement)**: labs exist; NBW plus wanted-signal transfer must be measured at equal gain/DC/CFO | `PREDEMOD_LAB.md` |
| P2 LP-core quality observer | **Open**: observer-only design (DC-corrected envelope, phase spread, origin/rail, raw sync) after an LP clock/latency benchmark | — |
| P2 Blocker sweep, P2 safe CVBS level update, emphasis | **Open (measurement / proven LUT path first)** | — |
| P3 Offline PLL/FMFB reference | **Open (offline)**: not executable on C5 at 40 MS/s | — |
| P0 Conducted attenuator baseline | **Open (bench)**: the only way to a measured dB total | — |

## Vendor PBUS / DC-search review (2026-10-07)

| Item | Status |
|---|---|
| `phy_pbus_workmode()` (= `phy_pbus_force_mode(0)`) forces gain index 50 for ~2 us and then clears the force bit when `0x600A9C18` bit 1 is set (verified by disassembly) | **Fixed**: every DC release re-forces the gain that was forced (`pbus_workmode_keep_gain`), so no path leaves the receiver unforced. The ~2 us G50 transient remains inside the gain change that triggers a release |
| DC hold / search re-asserted only PBUS blocks 0..3 in debug mode; the vendor drives 0..10 (5 GHz RF gain in block 8) | **Fixed**: all 11 blocks are saved, re-asserted and verified on rollback |
| Failed DC search could return codes loaded earlier for another gain | **Fixed**: the result is invalidated before each search; only this search's codes count |
| DC probe used unmeasured values after a failed measurement | **Fixed**: initialised; they only fed a log line |
| No-sync DC search could run on a weak carrier below sync detection | **Fixed**: two DC estimates 100 ms apart must agree (a carrier rotates the mean); refusals counted (`hw_dco_carrier_refusals`) |
| Measured RF gain routine | Not used: 2.4 GHz only in this binary |
| IQ image leakage under a strong neighbour | **Open (measurement)**: lab `~` recalibrates IQ at the tuned frequency; image rejection still to be measured with a blocker |

## Deeper Espressif PHY traces (2026-10-07)

"C5VRX: diepere Espressif-PHY-sporen voor bereik" (against `d6fc6fc`).

| § | Item | Status |
|---|---|---|
| 1 | No-sync DC search could treat a weak carrier as quiet | **Fixed**: an independent carrier test (`predemod_envelope_ratio_x100`: whole-capture DC removed, then envelope mean^2/var; noise ~100 with or without DC, carrier at 0 dB SNR ~146 in the host test) plus two agreeing DC estimates; rate-limited to once a second. All automatic calibration needs 5 s of uninterrupted confirmed quiet (operator: automatic, nothing odd between antenna swaps) |
| 2 | Unmeasured probe values; old codes taken as a new result | **Fixed**: `phy_rx_lab_dco_search()` with a result object (`measured`, `codes`, `residual`, `rolled_back`, id); a missing baseline/I/Q measurement goes straight to rollback; the previous result is invalidated at the start. Host fault injection: baseline, I-probe and Q-probe failures and a rollback failure after an earlier valid result |
| 3 | `phy_pbus_workmode()` G50 / unforced gain | **Fixed (re-force, no vendor write removed)**; the physical transition is still to be captured on the board |
| 4 | DCO rollback missed the 5 GHz RF word (block 8) | **Fixed + readback**: all vendor blocks 0..10 are re-asserted with their live words (debug mode freezes every block on its test register; a live copy keeps the state, leaving them would not); read-only RF/BB/fine words (8/1, 0/2, 1/2) traced at hold enter / in debug / after release + replay; the RF word is compared with the vendor tuple's RF code every 250 ms (`GAIN_PBUS`, `rf_mismatches`) |
| 5 | DCO cache without context | **Fixed**: blob v2 keyed on frequency, gain table range and band, lane policy, analog filter code/skirt and IQ-scale selector; per-gain residual as provenance; no boot epoch. Codes are absolute DC-DAC values, so a vendor recalibration does not invalidate them. Temperature not stored (no validated sensor path); stale entries are re-searched after 120 s of quiet |
| 6 | IQ image rejection | **Read-only state + automatic exact-frequency IQ calibration**: `IQ_STATE` prints `phy_param[44]` (rxiq_opt branch), `phy_param[650]` (scale selector), `0x600A043C`, `0x600A0438`; the vendor RX DC/IQ calibration at the tuned frequency is **lab only** (`~`): run automatically it changed the gain per index and caused a fine grain on strong pictures (see below). **Open (bench)**: IRR with a known tone at +-1/4/8/12 MHz and a blocker |

### Automatic exact-frequency recal removed (2026-10-07, A1 5865 MHz)

User report: fine grain over the whole picture with a strong VTX, `main`
clean. A/B on the board with the VTX on: after the quiet-gated
`RX_RECAL_AUTO` the same gain index gave a far larger envelope (G54: P50 72,
58 % clip; G62: P50 113, 100 % clip) than on `main` (G56: P50 13; G63:
P50 23); a boot that skipped the recal settled at main's gains (G31-33, P50
13-23, Q 99 %) and the picture was clean. `phy_set_rx_gain_table()`
reinstalls the gain memory with the new corrections, so the recal changes
the gain per index. The automatic run is removed; `~` remains a lab.

### Board results 2026-10-07 (VTX off, A1 5865 MHz)

- Exact-frequency RX recalibration (`RX_RECAL_AUTO`, ~49 ms, at every boot
  and channel change): receiver DC 1532/946 -> -283/115 mcells (earlier
  boots 2677/1720 -> 87/-479, 2073/1703 -> -533/739).
- Then the per-gain DC-DAC search over G83..G54 in confirmed quiet: all 30
  gains found codes; receiver DC at the table maximum 4/9 mcells.
- Receiver noise no longer reads as a carrier (no `[CARRIER]` lines), the
  idle raster enters and stays.
- Gain readback: hardware index = V5 index (0 mismatches); the PBUS RF word
  (block 8) reads 487, the RF code of the exact vendor tuple, and stays 487
  at hold enter / in debug mode / after release.
- `IQ_STATE`: rxiq_opt branch inactive (`phy_param[44]=0`), scale selector 0.
- Range-edge gear now `edge` (the V5 profile had forced BW40 at every boot).
- Fixed along the way: an idle-raster start that could reboot the receiver
  (1 ms busy-wait under ESP_ERROR_CHECK), the raster flickering on
  calibration transients, a status dump that starved IDLE.
- Still to do with a transmitter: picture at range, gain behaviour, and the
  sensitivity bench (P0 attenuator baseline).
