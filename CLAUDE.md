# Gresil — Claude Code context

## What this is
Mutable Instruments Beads (granular texture synthesizer) as an audio FX for
Ableton Move via Schwung. Sibling of Verglas (Clouds).

Plugin type: audio_fx · Module ID: `gresil` · API: `audio_fx_api_v2_t`
(entry `move_audio_fx_init_v2`, MIDI via dlsym'd `move_audio_fx_on_midi`)
Language: C++17 (DSP) with C linkage exports.

Read `design-spec.md` for the full design. This file is the compressed version.

## Layout
- `src/gresil_move.cpp`: wrapper (params, state, clock PLL, freeze-save worker)
- `src/beads/dsp/`: upstream Beads DSP with small patches marked "Gresil";
  `wet_filter.h` (wet-only HPF/LPF) is a Gresil addition
- `src/beads/resources.*`: upstream tables (lut_sine, wavetables)
- `src/stmlib/`: fresh stmlib (not Verglas' copy)
- `src/module.json`: single source of truth for chain_params + ui_hierarchy (DSP returns -1)
- `src/help.json`: on-device help
- `tests/test_gresil.cpp`: native tests (includes the wrapper .cpp directly)
- `scripts/build.sh` (ARM64 .so), `scripts/test.sh` (native tests), `scripts/install.sh`

## Build / test
```bash
docker build -t gresil-builder .        # runs tests, then the ARM64 build
docker cp $(docker create gresil-builder):/build/modules/gresil ./modules/
./scripts/install.sh [move-ip]
```
CI (`.github/workflows/release.yml`) runs tests + build on tag `vX.Y.Z`, which
must match `src/module.json` version.

## Critical implementation notes (never violate)
- Every entry point runs on the audio callback: no file I/O, malloc, locks or
  logging outside `create_instance`. File I/O lives in the freeze-store worker
  (SCHED_OTHER, cores 0-2, joined in destroy).
- The callback never touches the delay banks while `loading` is set (the
  worker is writing into them).
- Bump `store.generation` whenever the frozen buffer can change (unfreeze,
  media/buffer change, trim, load).
- `-DTEST` is required (stmlib portable paths). Never put printf under `TEST`
  in DSP code.
- The engine runs 16-frame sub-blocks; keep them even (2x decimating media).
- `Processor::Configure` clears the buffer: only call it when media or buffer changes.
- `get_param` returns -1 for unknown keys and raw values only (no units).
  Labels, units and pages live in module.json: Schwung 1.x never sends
  `_level` / `knob_N_*` to an audio FX, so do not add a knob-overlay layer.
- State JSON: every non-trigger param + `freeze_id`, values quoted; the parser
  is key-based and whitespace-tolerant (the host pretty-prints).
- Time reads across the recorded part of the buffer (trimmed on configure).
- Delay mode is the `mode` param, NOT Size at max. Granular caps size.base at
  0.975; delay mode passes size.base = 1 and the `filter` param as the Size
  attenuverter. Knob 2 on Beads/Random swaps via `visible_if`. Synth source
  ignores delay mode (`delay_mode()` checks the source).
- Pages: Beads, Random, Filters, Setup (ui_hierarchy in module.json).
- "Dry: Clean" must stay transparent on every media at Drive 0 (wow & flutter
  on the wet only, soft clip follows Drive).
  HPF/LPF are wet-only (WetFilter inside Processor/Synth, before the mix);
  Low Boost and the Limiter act on the whole output after the engine. No
  bypass branches in either stage.

## Move constraints
- 44100 Hz, 128 frames, int16 stereo; `-ffast-math`; files on device `ableton:users`
- Install path `/data/UserData/schwung/modules/audio_fx/gresil/`
- Freeze files: `/data/UserData/schwung/module_data/gresil/<freeze_id>.frz`
