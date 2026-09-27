# ESP32-C5 BitScrambler M2M speedlab

This is a boot-only **silicon throughput probe**, not an FM demodulator or a
working Phase5-360 mode. It runs once at boot **before** RF and video are
started; the normal Golden DAC pipeline starts afterward. It is C5-only and
does not change the 40 MS/s production ring or DAC pins.

The PR #87 artifact is built with `CONFIG_C5VRX_BS_SPEEDLAB=y` specifically
for the pull request and can be flashed via the experimental PR build. Read
the serial output lines starting with `BS_SPEEDLAB` immediately after boot;
they print before normal video begins. All other builds, including a future
merge into `main`, retain the production default `n`. For a separate local
experiment, explicitly set `CONFIG_C5VRX_BS_SPEEDLAB=y` before building.

For every attach selector supported by the current C5 headers (I2S0, GPSPI2,
UHCI, AES, SHA, ADC, PARLIO) the probe requests one loopback handle, loads
each of eight distinct programs, warms up once, then runs sixteen 8192-byte
memory-to-memory transforms. Each program reads 16 bits and emits 16 zero
bits per pair, with `K-1` idle bundles between transfers. Output length and
all bytes are verified before reporting a speed. Allocation, program load,
logging and first-run warmup are excluded from the timed interval, but each
finite driver transaction's reset/setup cost is **included**. If an attach
selector is unsupported or a short transfer occurs it reports the error and
does not report a misleading throughput.

`in_MBps_x100` is 100 times the decimal input throughput; `bundle_MHz_x100`
is 100 times `(input_bytes / 2) * K / elapsed_seconds / 1e6`. For example,
`bundle_MHz_x100=8000` corresponds to 80 million executed bundles per second
on this synthetic benchmark. The K scaling and minimum across multiple runs
matter more than one isolated peak. These are measured finite-call rates,
**not** a BitScrambler clock register readout.

| Result | Meaning for 20 million Phase5 output pairs/s |
| --- | --- |
| K=2 reaches 40 M bundles/s | Sanity check against existing two-bundle throughput. |
| K=3 stays above 60 M bundles/s | A three-bundle M2M kernel might have enough compute; ring arbitration remains untested. |
| K=4 stays above 80 M bundles/s | Four-bundle one-pass kernel becomes worth implementing; full RX + M2M + TX is still unproven. |
| Failure/short write/low rate | No live Phase5-360 claim; preserve Golden. |

This probe does not sweep GDMA priority/weights: the public loopback handle
does not expose its GDMA channel handles, so changing live DMA registers is
separate work after baseline timing. It does not claim hardware continuous
loopback or a PHY phase tap. The next gate, if an M2M attachment exceeds the
compute threshold, is concurrent PARLIO RX + M2M + plain PARLIO TX with ring
head tracking, no missed groups and continuous output through ring wraps.
