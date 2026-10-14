# Gresil — Design Spec

> **Status:** Scaffolded and building (v0.1.0). Not yet tested on hardware.
> **Plugin type:** audio_fx
> **Module ID:** `gresil`
> **Last updated:** 2026-10-12

---

## What it is

Mutable Instruments **Beads** (Emilie Gillet, 2021), the successor to Clouds,
ported to Ableton Move as an audio effect. Sibling to Verglas (Clouds): *verglas*
is black ice, *grésil* is the fine ice pellets that fall in grains.

Stereo audio is recorded continuously into a 768 KB buffer. Up to 32 grains
replay slices of it with their own position, length, envelope and pitch, and
each of those four can be randomized per grain. Turning Size fully clockwise
crossfades into a multitap delay with pitch-shifted taps and a filter, whose
time can lock to Move's clock. Four "media" (tape and digital) change the
recording's bit depth, sample rate and character. With Source set to Synth,
Gresil plays Beads' built-in wavetable oscillator and needs no input.

## Sonic intent

**References:** Mutable Instruments Beads, Clouds; granular clouds, frozen
textures, tape-delay smears, clock-synced glitch delays, degraded media.

**Philosophy:** Faithful port of the Beads DSP (the code in `src/beads/dsp` is
upstream with small, commented patches). Hardware-only pieces are replaced by
Move equivalents: pots become knobs, CV-less attenuverters become the Random
page, the SEED clock input becomes MIDI clock, the flash backup of a frozen
buffer becomes a file.

**Not:** Playable from pads (pad-fired seeds with V/Oct pitch were considered
and left out). Not a reimagining: no extra filters, EQ or limiter beyond Beads'.

---

## Source

- `pichenettes/eurorack` @ `f4cfa7697095a28d292bff758beb51deb49b5a86`, `beads/`
- `pichenettes/stmlib` @ `d18def816c51d1da0c108236928b2bbd25c17481`
- Both MIT. stmlib is a fresh copy, not Verglas' older one: Beads needs
  `HysteresisQuantizer2`, `hysteresis_filter.h`, `sample_rate_converter.h`,
  `gate_flags.h`.

## DSP architecture

```
int16 in → drive + media soft clip → [16-frame sub-blocks] →
  Source=Input: beads::Processor
    TriggerGenerator (granular) | DelayController (delay, Size > 0.99)
    → GranularProcessor<resolution, size, ratio> (feedback, limiter, DC block,
      2x decimation for tape media) → 32 DelayGrains | 8x2 Taps + LP/HP
    → dry/wet (equal-power-ish) → wow & flutter (Dirty Tape)
    → pre-reverb limiter → Griesinger/Dattorro reverb → output limiter
  Source=Synth: beads::Synth (wavetable voice + 32 wavetable grains → reverb)
→ fade-in after reconfiguration → int16 out
```

### Media on a fixed 44.1 kHz host

| Media | Hardware | Gresil | Buffer (stereo / mono) |
|---|---|---|---|
| Dirty Tape | 8-bit µ-law, codec at 24 kHz | 8-bit µ-law, 2x decimated (22.05 kHz) + wow & flutter LP 0.6 (upstream's non-`ENABLE_HARDWARE_24K` path) | 17.4 s / 34.8 s |
| Clean Tape | 12-bit, 48 kHz codec, 2x decimated | 12-bit, 2x decimated (22.05 kHz) | 11.6 s / 23.2 s |
| Vintage | 12-bit, codec at 32 kHz | 12-bit at 44.1 kHz + 4th-order Butterworth LP at 14.5 kHz on the wet path (32 kHz is not an integer ratio of 44.1) | 5.8 s / 11.6 s |
| Digital | 16-bit, 48 kHz | 16-bit, 44.1 kHz | 4.35 s / 8.7 s |

### Patches to upstream (all marked "Gresil" in the code)

- `processor.h`: `ENABLE_HARDWARE_24K` off; `sample_rate()` returns 44100 for
  every media; `Trim()` gets its missing `break`s; `set_clean_dry()`; the
  Vintage low-pass filters.
- `processor.cc`: processors init at 44100 / 22050; reverb and wow & flutter at
  44100; clean dry path at unity gain when `clean_dry_` (default). The
  hardware dry is media-degraded and scaled by 0.75, which makes Mix = 0
  non-transparent in a Move chain. "Dry: Media" restores it.
- `synth.cc`: 48000 → 44100.
- `wavetable_granular_player.h`: a debug `printf` was guarded by `TEST`, which
  the port must define for stmlib's portable code paths. Now guarded by
  `BEADS_DEBUG_GRAIN_STEALING`.

### Control rate

Beads runs 12-sample blocks (4 kHz). Several per-block coefficients (density
and FM hysteresis, the delay controller's FM smoothing) assume that rate, so
the wrapper processes Move's 128 frames as 8 sub-blocks of 16 (2.76 kHz,
even-sized for the 2x decimating media). Continuous knobs get a one-pole
smoother per sub-block (~4 ms).

### Wrapper behaviour (review fixes, 2026-10-12)

- **Defaults**: Size 0.5 + Density 0.75 gave 0.17 overlapping grains on
  average (mostly silence). Defaults are now Time 0.2 / Size 0.75 /
  Density 0.8, about 2.6 overlapping grains (`test_default_grain_density`).
- **Trim on configure**: `Configure()` clears the buffer but leaves its
  length at the maximum, so Time and Time Rnd pointed up to 35 s into silence
  after any media change or at load. The wrapper calls `Trim()` (the
  hardware's Media+Freeze) after each configure, except before a freeze
  restore. Time then spans what has been recorded, growing to the full buffer.
- **Drive**: gain and soft-clip blend are smoothed per sample (5 ms). It
  clicked when applied per block in 0.5 dB steps.
- **Media / Buffer / Source changes**: 10 ms fade out, reconfigure in a block
  of its own (output silent), 10 ms fade in.
- **Denormals**: FPCR.FZ is set for the duration of `process_block`.
  `-ffast-math` only enables flush-to-zero for executables, not for a `.so`.
- **Explicit Mode + Filter**: on the hardware, Size at max enters delay mode
  and the Size attenuverter becomes the delay filter. On Move the static knob
  labels hid that: a set with Size 100% and Size Rnd +100% sounded "horribly
  high-passed" (a resonant HP at about 2.7 kHz). Mode is now a param, and
  Filter is its own key. Knob 2 of the Beads and Random pages swaps via
  `visible_if` on `mode` (checked with Schwung's `planPages`). Older states
  migrate: Size > 0.99 → Delay, Size Rnd → Filter.
- **Random knobs** verified (`test_random_knobs`): Rnd 0 gives a fixed pitch
  and start; +1 spreads pitch about 0.3x–3.3x and start positions over about
  4 s; −1 gives sporadic jumps of the same reach.

### Memory and CPU

- Instance ≈ 860 KB: 3 × 256 KB delay banks (shared by all four media
  processors, as on the hardware) + 64 KB reverb + engine state.
- Native x86 worst block 164 µs (Dirty Tape mono granular) against a 2902 µs
  budget. Expect roughly 2–4x that on the Cortex-A72; on-device profiling is
  still to do.

---

## Parameters

### Page 1 — Beads (knobs 1–8)

| Key | Name | Range | Default | Notes |
|---|---|---|---|---|
| time | Time | 0–1 | 0.2 | Grain start position across the recorded part of the buffer (see Trim-on-configure). Delay mode: slice (frozen) / tap multiplier |
| size | Size | 0–1 | 0.75 | Granular only. V-shaped: longest at both ends (4 s), shortest (~8 ms) at 1/3; < 1/3 plays in reverse. 0.5 ≈ 33 ms, 0.75 ≈ 370 ms. Capped at 0.975 internally so it never enters delay mode. **Delay mode: this knob is Filter** |
| shape | Shape | 0–1 | 0.5 | Grain envelope (square → decay → bell → reverse). Delay: tap envelope |
| density | Density | 0–1 | 0.8 | < 0.48 regular, 0.48–0.52 off, > 0.52 random. Clock seed: < 0.48 divides the clock (1–16), > 0.52 probability |
| pitch | Pitch | −24..+24 st | 0 | Hardware pot has sticky intervals; here every semitone is reachable |
| feedback | Feedback | 0–1 | 0 | Synth source: wavetable bank 1–12 |
| reverb | Reverb | 0–1 | 0 | |
| dry_wet | Mix | 0–1 | 0.5 | Synth source: oscillator ↔ grains |

### Page 2 — Random

| Key | Name | Range | Default | Notes |
|---|---|---|---|---|
| time_rnd / size_rnd / shape_rnd / pitch_rnd | … Rnd | −1..+1 | 0 | The four attenuverters with nothing patched. Beads' pot law: dead zone at noon, quadratic. +: uniform per-grain spread (random walk in delay mode); −: occasional random jumps. **Delay mode: the Size Rnd knob is Filter** |
| freeze | Freeze | Off/On | Off | Stops recording; grains play the frozen buffer |
| media | Media | Dirty Tape / Clean Tape / Vintage / Digital | Digital | Changing it clears the buffer (as on the hardware) |
| seed | Seed | Free / Clock | Free | Clock: MIDI clock drives the SEED input with the button locked |
| clock_div | Clock Div | 1/32 … 1 Bar | 1/8 | Gate rate fed to SEED. In delay mode the delay time is a ratio of this interval |

### Page 3 — Filters (from Verglas)

| Key | Name | Range | Default | Notes |
|---|---|---|---|---|
| filter_hp | HPF | 0–1000 Hz | 0 (Off) | One-pole 6 dB/oct, **wet only**: applied in `Processor` / `Synth` before the dry/wet mix (and outside the feedback loop) |
| filter_lp | LPF | 1000–20000 Hz | 20000 (Off) | Same, low-pass, wet only |
| drive | Drive | 0–24 dB | 0 | Moved here from Setup (input gain, per-media soft clip) |
| limiter_pre | Pre Gain | −6..+6 dB | 0 | Into the tape limiter |
| limiter_post | Post Gain | −6..+6 dB | 0 | Out of the tape limiter |
| low_boost | Low Boost | 0–6 dB | 0 | Low-shelf biquad on the whole output, as in Verglas |
| low_freq | Low Freq | 30–400 Hz | 100 | |
| low_q | Low Q | 0.1–4 | 0.7 | |

Everything runs all the time (no bypass branches): "HPF Off" tracks at 5 Hz,
"LPF Off" passes straight through, the shelf is unity at 0 dB, and the
limiter crossfades in and out over 10 ms. Cutoffs, gains and the shelf
follow the per-sub-block smoothed values.

### Page 4 — Setup (Mode is knob 1, Limiter on/off is knob 4)

| Key | Name | Range | Default | Notes |
|---|---|---|---|---|
| mode | Mode | Granular / Delay | Granular | Explicit engine switch. Hardware: Size fully clockwise. Changing it runs Beads' own 1.5 s crossfade |
| filter | Filter | −1..+1 | 0 | Delay mode only (replaces Size on the Beads page and Size Rnd on the Random page): − low-pass, + high-pass, both resonant, Beads' attenuverter law. Hardware: the Size attenuverter in delay mode |
| source | Source | Input / Synth | Input | Synth: Beads' wavetable voice. Time scans the wave, Feedback picks the bank |
| buffer | Buffer | Stereo / Mono | Stereo | Mono sums L+R and doubles the recording time (hardware: auto-detected from patching) |
| limiter_on | Limiter | Off/On | Off | Verglas tape limiter (Chowdhury cubic + Airwindows IronOxide) on the final output; gains on the Filters page |
| dry | Dry | Clean / Media | Clean | Media: hardware dry path (degraded, −2.5 dB) |
| mono_grains | Mono Grain | Off/On | Off | Beads' hidden monophonic mode (each grain cuts the previous) |
| freeze_save | Frz Save | Off/On | On | Save a frozen buffer with the set |
| trim | Trim | trigger | — | Hardware Media+Freeze: restart the buffer from now and unfreeze |

Dynamic knob labels (overlay): Slice / Envelope / Delay Time or Ratio / Filter
in delay mode; Wave / Bank / Osc/Grain with the synth source. Size shows
"DELAY", Density shows "Reg n% / Off / Rnd n%".

---

## Feature: clock seed (tempo sync)

- MIDI clock (24 PPQN, `0xF8`) reaches audio FX through the dlsym'd
  `move_audio_fx_on_midi`, along with Start/Continue/Stop.
- Ticks arrive at block granularity (±128 samples), but Beads' `DelayClock`
  treats > 1 ms of jitter as a tempo change. So a phase-locked phasor runs at
  the smoothed tempo (`host->get_bpm()` when present, otherwise measured one
  beat at a time). It is nudged towards the tick grid with gain 0.01 and
  hard-resynced on > 1/4 beat error. `host->get_beat_position()`, when
  present, aligns divisions to the bar. Test: worst edge-interval error
  16 samples (0.36 ms) at 120 BPM.
- The phasor renders a 50% gate at Clock Div and becomes Beads' SEED input
  with the button locked. Upstream does the rest: the trigger divider and
  probability in granular mode, and the synchronized `DelayController` (ratios
  1/16..1 of the clock interval) in delay mode.
- Transport stopped (Stop received, host clock status STOPPED, or no tick for
  0.5 s): falls back to Free behaviour.

## Feature: freeze save (buffer persistence)

Hardware: 10 s after Freeze engages, the buffer is backed up to flash, and the
module boots frozen with it. Gresil:

- After 2 s of stable freeze, the callback snapshots the header (write pointer,
  media, mono) and posts a save request. A worker thread (created at the end of
  `create_instance`, demoted to SCHED_OTHER and pinned to cores 0–2) writes
  `<data_dir>/<freeze_id>.frz` = 20-byte header + 786432 bytes, through a
  `.tmp` + `rename`.
- A generation counter, bumped on unfreeze, media/buffer change, trim and load,
  aborts a save that could capture a torn buffer (checked per 64 KB chunk and
  before the rename).
- `freeze_id` is a random hex id stored in the module state. When a state with
  `freeze: On` is restored, the callback configures the media, pauses the
  engine (the input passes through), and the worker reads the file straight
  into the delay banks. The write pointer is applied, then a 20 ms fade-in.
  No valid file means it wakes up unfrozen (hardware behaviour).
- `data_dir` = `/data/UserData/schwung/module_data/gresil`. The
  `GRESIL_DATA_DIR` env var overrides it for tests.
- No file I/O, malloc or locks on the audio callback; the worker is joined in
  `destroy_instance`.
- Known limits: files are never deleted (one per instance that was ever
  frozen, 768 KB each). A duplicated set shares the id, so re-freezing in one
  copy overwrites the other's backup.

## Feature: synth source

`beads::Synth` is what the hardware plays when nothing is patched for 10 s.
Here it's an explicit Source switch instead, because an auto switch would
start a drone on any track that goes silent. It is unchanged apart from the
sample rate. The trigger generator and Clock seed work the same way. Freeze
holds the wavetable position. Freeze save is disabled with this source (the
synth has no buffer).

---

## Hardware constraints (Move)

- 44100 Hz, 128-frame blocks, int16 stereo interleaved; `-ffast-math` (no FTZ)
- All entry points run on the SPI audio callback: no I/O, malloc, locks or
  logging outside `create_instance`
- Glibc 2.34 (pthreads), same as Mello, Magneto, Forge
- `chain_params` / `ui_hierarchy` served from module.json (DSP returns −1)

## Open questions

- [ ] On-device CPU profile per media and per mode (32 grains + Hermite reads)
- [ ] Confirm Start/Stop/ticks reach the audio FX `on_midi` on the target host
      without "MIDI Clock Out", and what `get_beat_position` returns
- [ ] Is the Knob Grid happy with the dynamic `knob_N_name` labels, or should
      delay mode use `visible_if` pages instead?
- [ ] Freeze file cleanup (delete on unfreeze? age-based sweep?)
- [ ] Is 2 s the right freeze-save delay (hardware: 10 s)?
- [ ] Vintage media: is a 14.5 kHz LP close enough to the 32 kHz codec, or is a
      fractional resampler worth it?
