# Golden hard guard (experimental)

The default 6BIT@40 path on this branch uses `fm_golden_hard_guard.bsasm`.
For an endpoint phase step of 0 through 11 Phase5 bins, it emits the exact
Golden DAC value. For 12 through 16 bins, it emits DAC pedestal 20, discards
that phase candidate, and reseeds history from the next endpoint. The next
output is also pedestal while the new pair is formed. Every input pair still
consumes two bytes and emits two identical DAC bytes. Output is delayed by one
50 ns pair compared with Golden. The 4BIT@80 path continues to use `fm4`.

The USB console command `z` prints and resets a histogram of absolute phase
steps from the control capture: `C5VRX_DELTA_HIST` summarizes counts at or
above bins 9, 12, and 15; `C5VRX_DELTA_BIN` prints each bin from 0 to 16.
`strong` counts steps for which all three samples passed the existing strong
power threshold. `gain_writes` shows how many gain changes happened during
the measurement period. The capture covers a short window about every 50 ms,
so counts estimate the sampled signal rather than every video sample.

To compare thresholds, hold the transmitter scene and receiver placement
steady, collect several `z` snapshots with stable gain, and inspect live
color and sync. The histogram alone cannot establish a universal color-safe
threshold. The route on this branch is the mainline Q[9:6]/I[9:6] route;
the separate Q[6:3]/I[6:3] experiment is not included.

Generate and verify the program with
`python tools/gen_golden_hard_guard.py` and
`python tools/test_golden_hard_guard.py`.
