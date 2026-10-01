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

The test calls `mcpx_apu_monitor_mixdown`, the function `mcpx_apu_dsp_frame` hands
the mixbins to. Since the GP DSP port the frame function first writes the mixbins
into GP DSP memory, which needs a GP DSP the test does not create; calling the
frame function crashed on a NULL `d->gp.dsp`.

## Running

    cmake -S tests/apu_mixdown -B build/apu-mixdown
    cmake --build build/apu-mixdown
    ctest --test-dir build/apu-mixdown

Three tests. `apu_mixdown_all` (the default 5.1 fold-down) puts a signal only
in bins 6..10 and requires it to reach the frame buffer on both channels, then
requires a centre-only signal to reach both channels equally.
`apu_mixdown_even_odd` checks the first wide mixdown (`RECOMP_APU_MIXDOWN_ALL=2`,
even bins left, odd bins right). `apu_mixdown_two_bins` requires the same
signal to be *lost* with the switch off — without that arm, a mixdown that
ignored its own switch would pass.
