# Gresil

Mutable Instruments **Beads** granular texture synthesizer as an audio FX for
Ableton Move, built for the [Schwung](https://github.com/charlesvestal/schwung) framework.

Up to 32 grains with per-grain randomization, a delay mode at maximum Size,
four recording media (Dirty Tape, Clean Tape, Vintage, Digital), freeze, and
reverb. Beads' DSP is ported unchanged apart from the sample rate. On top of
that:

- **Clock seed**: grains and delay taps follow Move's MIDI clock
- **Freeze save**: a frozen buffer is saved with the set and comes back on reload
- **Synth source**: Beads' built-in wavetable voice, no input needed

## Install

```bash
docker build -t gresil-builder .
docker cp $(docker create gresil-builder):/build/modules/gresil ./modules/
./scripts/install.sh            # deploys to Move via SSH
```

## Pages

| Knob | Beads | Random | Filters | Setup |
|---|---|---|---|---|
| 1 | Time | Time Rnd | HPF (wet only) | Mode (Granular/Delay) |
| 2 | Size (Delay: Filter) | Size Rnd (Delay: Filter) | LPF (wet only) | Source |
| 3 | Shape | Shape Rnd | Drive | Buffer |
| 4 | Density | Pitch Rnd | Pre Gain | Limiter |
| 5 | Pitch | Freeze | Post Gain | Dry |
| 6 | Feedback | Media | Low Boost | Mono Grain |
| 7 | Reverb | Seed (Free/Clock) | Low Freq | Frz Save |
| 8 | Mix | Clock Div | Low Q | Trim |

The Filters page comes from Verglas. HPF and LPF only shape the effect (the
dry signal is untouched); Low Boost and the Limiter act on the whole output.

## Credits

- **DSP**: Emilie Gillet / [Mutable Instruments](https://github.com/pichenettes/eurorack) (MIT)
- **Move port**: filliformes

## License

DSP code: MIT (Emilie Gillet). Port and wrapper: MIT.
