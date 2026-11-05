# apu_mixdown

`mcpx_apu_dsp_frame` used to read mixbins 0 and 1 and discard 2..31. On
hardware the GP and EP mix the submixes down; here they are stubs, so thirty
of thirty-two bins were computed correctly and thrown away every frame, with
no counter anywhere to say so.

Titles route their 3D positional voices — their sound effects — to bins above
1. Measured on Jet Set Radio Future over one 200 s gameplay run:

    [APU-BIN] 2D heard=557466 lost=0
              3D heard=0      lost=377768
              lost by bin: 6,7,8,9,10

Music is on 2D voices and lands in bins 0 and 1, which is why it was always
audible while no effect ever was.

## Running

    cmake -S tests/apu_mixdown -B build/apu-mixdown
    cmake --build build/apu-mixdown
    ctest --test-dir build/apu-mixdown

## Gain staging

Summing all 32 bins at unity then made JSRF's character sounds 14-15 dB louder
than its music (bins 6/7 at -11 dBFS RMS, peaks at full scale, 0.37% of
samples clipped, against music at -24 to -27). The title's own GP effects image
(the standard dsstdfx), run on a DSP56300 for one gameplay capture, passes the
speaker bins at unity, the crosstalk bins 6-9 at -6.0 dB, and nothing of the
reverb send in bin 10. The default mixdown now does the same: bins 0-5 at
unity, 6-9 at half, 10 dropped. Bins 11-31 stay at unity: no capture measured
them, and Burnout 3's sends to 17/18 sit only about 5 dB below its main bins,
so dropping them cost its race 0.9 dB. `apu_mixdown_all=2` keeps the unity sum
and `=0` the two-bin read.

Three tests, one per mode, each with exact expected levels for a signal in a
speaker pair, a crosstalk pair, the reverb send and an effect send (bin 17). `apu_mixdown_two_bins` is
the negative control: without it, a mixdown that ignored its own switch would
pass.
