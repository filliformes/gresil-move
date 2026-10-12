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

| Knob | Beads | Random | Setup |
|---|---|---|---|
| 1 | Time | Time Rnd | Mode (Granular/Delay) |
| 2 | Size (Delay: Filter) | Size Rnd (Delay: Filter) | Source |
| 3 | Shape | Shape Rnd | Buffer |
| 4 | Density | Pitch Rnd | Drive |
| 5 | Pitch | Freeze | Dry |
| 6 | Feedback | Media | Mono Grain |
| 7 | Reverb | Seed (Free/Clock) | Frz Save |
| 8 | Mix | Clock Div | Trim |

## Credits

- **DSP**: Emilie Gillet / [Mutable Instruments](https://github.com/pichenettes/eurorack) (MIT)
- **Move port**: fillioning

## License

DSP code: MIT (Emilie Gillet). Port and wrapper: MIT.
