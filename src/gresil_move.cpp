// Gresil — Mutable Instruments Beads as an audio FX for Ableton Move (Schwung)
// MIT License (Emilie Gillet DSP, fillioning port)
//
// Wraps beads::Processor (granular + delay engines, 4 media) and beads::Synth
// (the wavetable voice Beads plays when nothing is patched) into the
// audio_fx_api_v2 interface. Additions over the hardware:
//   - Clock seed: grains / delay taps locked to Move's MIDI clock
//   - Freeze save: a frozen buffer is written to disk in the background and
//     reloaded with the set, like the hardware's flash backup
//   - Source: switch between the audio input and the wavetable synth
//
// Audio: 44100 Hz, 128 frames/block, stereo interleaved int16. The Beads
// engine runs in 16-frame sub-blocks to stay close to its 4kHz control rate.
//
// Knob pages, labels and units come from module.json (chain_params +
// ui_hierarchy). Schwung >= 1.0 reads and writes params by key; it never sends
// _level or knob_N_* to an audio FX, so this file has no knob-overlay layer.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <pthread.h>
#include <sched.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "beads/dsp/processor.h"
#include "beads/dsp/synth.h"
#include "stmlib/dsp/dsp.h"
#include "stmlib/utils/gate_flags.h"

using beads::FloatFrame;

// ---- Schwung API headers (inline, must match chain_host ABI exactly) ----
extern "C" {

typedef int (*move_mod_emit_value_fn)(void *ctx,
                                      const char *source_id,
                                      const char *target,
                                      const char *param,
                                      float signal,
                                      float depth,
                                      float offset,
                                      int bipolar,
                                      int enabled);
typedef void (*move_mod_clear_source_fn)(void *ctx, const char *source_id);

typedef struct host_api_v1 {
    uint32_t api_version;
    int sample_rate;
    int frames_per_block;
    uint8_t *mapped_memory;
    int audio_out_offset;
    int audio_in_offset;
    void (*log)(const char *msg);
    int (*midi_send_internal)(const uint8_t *msg, int len);
    int (*midi_send_external)(const uint8_t *msg, int len);
    int (*get_clock_status)(void);
    move_mod_emit_value_fn mod_emit_value;
    move_mod_clear_source_fn mod_clear_source;
    void *mod_host_ctx;
    float (*get_bpm)(void);
    int (*midi_inject_to_move)(const uint8_t *msg, int len);
    int (*slot_recv_channel)(void *instance);
    double (*get_beat_position)(void);
    void *reserved[8];
} host_api_v1_t;

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

}  // extern "C"

static const host_api_v1_t *g_host = NULL;

// ---- Constants ----
static const int kMaxFrames = 128;
static const int kSubBlock = 16;           // ~2.7kHz control rate (hardware: 4kHz)
static const float kSampleRate = 44100.0f;
static const size_t kBankSize = 262144;    // Beads splits its RAM in 3 banks
static const size_t kNumBanks = 3;
static const size_t kDelayMemorySize = kBankSize * kNumBanks;
static const size_t kReverbBufferSize = 32768;
static const float kFreezeSaveDelay = 2.0f;  // s of stable freeze before saving
static const float kFadeTime = 0.01f;        // s, fade out/in around a media/source change
static const float kDriveSmoothing = 0.0045f; // per sample, ~5 ms
static const int kClockStaleSamples = 22050; // 0.5 s without a tick = stopped

static const char *kDefaultDataDir = "/data/UserData/schwung/module_data/gresil";

#define MOVE_CLOCK_STATUS_STOPPED 1

// ---- Parameters ----
enum ParamId {
    P_TIME, P_SIZE, P_SHAPE, P_DENSITY, P_PITCH, P_FEEDBACK, P_REVERB, P_MIX,
    P_TIME_RND, P_SIZE_RND, P_SHAPE_RND, P_PITCH_RND,
    P_FREEZE, P_MEDIA, P_SEED, P_CLOCK_DIV,
    P_SOURCE, P_BUFFER, P_DRIVE, P_DRY, P_MONO_GRAINS, P_FREEZE_SAVE,
    P_MODE, P_FILTER,
    P_FILTER_HP, P_FILTER_LP, P_LIMITER_ON, P_LIMITER_PRE, P_LIMITER_POST,
    P_LOW_BOOST, P_LOW_FREQ, P_LOW_Q,
    P_TRIM,
    P_COUNT
};

enum ParamType { T_FLOAT, T_INT, T_ENUM, T_TRIGGER };

static const char *OPT_OFF_ON[] = { "Off", "On" };
static const char *OPT_MEDIA[] = { "Dirty Tape", "Clean Tape", "Vintage", "Digital" };
static const char *OPT_SEED[] = { "Free", "Clock" };
static const char *OPT_CLOCK_DIV[] = {
    "1/32", "1/16T", "1/16", "1/8T", "1/8", "1/4T", "1/4", "1/2", "1 Bar"
};
static const float CLOCK_DIV_BEATS[] = {
    0.125f, 1.0f / 6.0f, 0.25f, 1.0f / 3.0f, 0.5f, 2.0f / 3.0f, 1.0f, 2.0f, 4.0f
};
static const char *OPT_SOURCE[] = { "Input", "Synth" };
static const char *OPT_BUFFER[] = { "Stereo", "Mono" };
static const char *OPT_DRY[] = { "Clean", "Media" };
static const char *OPT_MODE[] = { "Granular", "Delay" };

struct ParamDef {
    const char *key;
    const char *name;
    ParamType type;
    float min, max, step, def;
    const char *const *options;
    int num_options;
};

#define ENUM_DEF(k, n, opts, d) \
    { k, n, T_ENUM, 0, (float)(sizeof(opts) / sizeof(opts[0]) - 1), 1, d, opts, \
      (int)(sizeof(opts) / sizeof(opts[0])) }

static const ParamDef PARAMS[P_COUNT] = {
    // Size is V-shaped (shortest grains, ~8 ms, at 1/3), so its default sits
    // well right of noon: 0.75 = ~370 ms grains. With Density 0.8 (~7 random
    // grains/s) about 4 grains overlap (0.5 / 0.75 averaged 0.17: mostly silence).
    { "time",        "Time",      T_FLOAT, 0,   1,  0.01f, 0.2f,  NULL, 0 },
    { "size",        "Size",      T_FLOAT, 0,   1,  0.01f, 0.75f, NULL, 0 },
    { "shape",       "Shape",     T_FLOAT, 0,   1,  0.01f, 0.5f,  NULL, 0 },
    { "density",     "Density",   T_FLOAT, 0,   1,  0.01f, 0.8f,  NULL, 0 },
    { "pitch",       "Pitch",     T_INT,   -24, 24, 1,     0,     NULL, 0 },
    { "feedback",    "Feedback",  T_FLOAT, 0,   1,  0.01f, 0,     NULL, 0 },
    { "reverb",      "Reverb",    T_FLOAT, 0,   1,  0.01f, 0,     NULL, 0 },
    { "dry_wet",     "Mix",       T_FLOAT, 0,   1,  0.01f, 0.5f,  NULL, 0 },
    { "time_rnd",    "Time Rnd",  T_FLOAT, -1,  1,  0.01f, 0,     NULL, 0 },
    { "size_rnd",    "Size Rnd",  T_FLOAT, -1,  1,  0.01f, 0,     NULL, 0 },
    { "shape_rnd",   "Shape Rnd", T_FLOAT, -1,  1,  0.01f, 0,     NULL, 0 },
    { "pitch_rnd",   "Pitch Rnd", T_FLOAT, -1,  1,  0.01f, 0,     NULL, 0 },
    ENUM_DEF("freeze",      "Freeze",     OPT_OFF_ON,    0),
    ENUM_DEF("media",       "Media",      OPT_MEDIA,     3),
    ENUM_DEF("seed",        "Seed",       OPT_SEED,      0),
    ENUM_DEF("clock_div",   "Clock Div",  OPT_CLOCK_DIV, 4),
    ENUM_DEF("source",      "Source",     OPT_SOURCE,    0),
    ENUM_DEF("buffer",      "Buffer",     OPT_BUFFER,    0),
    { "drive",       "Drive",     T_FLOAT, 0,   24, 0.5f,  0,     NULL, 0 },
    ENUM_DEF("dry",         "Dry",        OPT_DRY,       0),
    ENUM_DEF("mono_grains", "Mono Grain", OPT_OFF_ON,    0),
    ENUM_DEF("freeze_save", "Frz Save",   OPT_OFF_ON,    1),
    // Gresil: on the hardware, delay mode is "Size fully clockwise" and the
    // Size attenuverter doubles as the delay filter. On Move both are explicit.
    ENUM_DEF("mode",        "Mode",       OPT_MODE,      0),
    { "filter",      "Filter",    T_FLOAT, -1,  1,  0.01f, 0,     NULL, 0 },
    // Filters page, from Verglas. HPF/LPF act on the wet signal only (inside
    // the processor, before the dry/wet mix); the low shelf and the tape
    // limiter act on the final output, as in Verglas.
    { "filter_hp",   "HPF",       T_FLOAT, 0,   1000,  10,   0,     NULL, 0 },
    { "filter_lp",   "LPF",       T_FLOAT, 1000, 20000, 10,  20000, NULL, 0 },
    ENUM_DEF("limiter_on",  "Limiter",    OPT_OFF_ON,    0),
    { "limiter_pre", "Pre Gain",  T_FLOAT, -6,  6,  0.5f,  0,     NULL, 0 },
    { "limiter_post","Post Gain", T_FLOAT, -6,  6,  0.5f,  0,     NULL, 0 },
    { "low_boost",   "Low Boost", T_FLOAT, 0,   6,  0.5f,  0,     NULL, 0 },
    { "low_freq",    "Low Freq",  T_FLOAT, 30,  400, 10,   100,   NULL, 0 },
    { "low_q",       "Low Q",     T_FLOAT, 0.1f, 4, 0.1f,  0.7f,  NULL, 0 },
    { "trim",        "Trim",      T_TRIGGER, 0, 1,  1,     0,     OPT_OFF_ON, 2 },
};

// Beads' delay crossfade starts above size 0.99 (0.98 once in delay mode).
// Granular mode keeps Size just below it; delay mode drives it to the top.
static const float kGranularMaxSize = 0.975f;

// ---- Clock sync ----
// MIDI clock (24 PPQN) arrives at block granularity, so ticks are jittered by
// up to one block. A phase-locked phasor runs at the smoothed tempo and is
// nudged towards the tick grid, which gives Beads' DelayClock edges that are
// stable to well under its 1ms jitter tolerance.
struct ClockSync {
    int stopped;             // an explicit Stop was received since the last Start
    uint32_t tick_count;     // ticks since Start (or since the instance began)
    double phase;            // beats (PLL output)
    double grid;             // beat position of the last tick
    float samples_per_beat;  // smoothed tempo
    int samples_since_beat_tick;  // samples since tick_count was a multiple of 24
    int has_period;
    int stale;               // samples since the last tick
};

static void clock_init(ClockSync *c) {
    memset(c, 0, sizeof(*c));
    c->samples_per_beat = kSampleRate * 0.5f;  // 120 BPM until measured
    c->stale = kClockStaleSamples;
}

// Beat position of the current tick. The host's song position is preferred
// (it keeps divisions aligned to the bar when the instance joins mid-song);
// otherwise ticks are counted from Start.
static double clock_grid_position(const ClockSync *c) {
    if (g_host && g_host->get_beat_position) {
        double b = g_host->get_beat_position();
        if (b == b && b >= 0.0 && b < 1.0e7) return b;
    }
    return c->tick_count == (uint32_t)-1 ? 0.0 : c->tick_count / 24.0;
}

static void clock_on_realtime(ClockSync *c, uint8_t status) {
    switch (status) {
        case 0xFA:  // Start
            // The first tick after Start is beat 0.
            c->stopped = 0;
            c->tick_count = (uint32_t)-1;
            c->phase = c->grid = 0.0;
            c->samples_since_beat_tick = 0;
            c->stale = 0;
            break;
        case 0xFB:  // Continue
            c->stopped = 0;
            break;
        case 0xFC:  // Stop
            c->stopped = 1;
            break;
        case 0xF8: {  // Tick
            const bool resync = c->stale >= kClockStaleSamples;
            c->stale = 0;
            ++c->tick_count;
            if ((c->tick_count % 24) == 0) {
                // Measure a whole beat at a time: block quantization error
                // drops to 128 samples per beat instead of per tick.
                float measured = (float)c->samples_since_beat_tick;
                if (!resync && measured > kSampleRate * 0.06f && measured < kSampleRate * 3.0f) {
                    if (!c->has_period) {
                        c->samples_per_beat = measured;
                        c->has_period = 1;
                    } else {
                        c->samples_per_beat += 0.1f * (measured - c->samples_per_beat);
                    }
                }
                c->samples_since_beat_tick = 0;
            }
            // PLL: pull the phasor towards the tick grid.
            c->grid = clock_grid_position(c);
            double error = c->grid - c->phase;
            if (resync || fabs(error) > 0.25) {
                c->phase = c->grid;
            } else {
                c->phase += 0.01 * error;
            }
            break;
        }
        default:
            break;
    }
}

static inline int clock_is_live(const ClockSync *c) {
    if (c->stale >= kClockStaleSamples) return 0;
    if (g_host && g_host->get_clock_status) {
        if (g_host->get_clock_status() == MOVE_CLOCK_STATUS_STOPPED) return 0;
    }
    return !c->stopped;
}

// Renders a gate stream (high for the first half of each division) and
// advances the phasor. Returns 0 when the clock is not running.
static int clock_render(ClockSync *c, float division_beats, bool *gate, int frames) {
    int live = clock_is_live(c);
    float spb = c->samples_per_beat;
    if (g_host && g_host->get_bpm) {
        float bpm = g_host->get_bpm();
        if (bpm >= 20.0f && bpm <= 999.0f) spb = kSampleRate * 60.0f / bpm;
    }
    const double increment = 1.0 / spb;
    const double limit = c->grid + 1.0 / 24.0;
    for (int i = 0; i < frames; ++i) {
        if (live) {
            double position = c->phase / division_beats;
            gate[i] = (position - floor(position)) < 0.5;
            // Never run more than a tick ahead of the grid when ticks stall.
            if (c->phase + increment < limit) c->phase += increment;
        } else {
            gate[i] = false;
        }
    }
    c->samples_since_beat_tick += frames;
    if (c->stale < kClockStaleSamples) c->stale += frames;
    return live;
}

// ---- Freeze persistence ----
// File I/O never happens on the audio callback. The callback posts a request
// and a SCHED_OTHER worker (cores 0-2) writes or reads the buffer. A
// generation counter, bumped whenever the frozen buffer may change, lets the
// worker abort a save that would capture a torn buffer.
enum StoreRequest { REQ_NONE = 0, REQ_SAVE = 1, REQ_LOAD = 2 };

struct FreezeFileHeader {
    uint32_t magic;      // 'GRSL'
    uint32_t version;
    int32_t write_ptr;
    uint8_t media;
    uint8_t mono;
    uint8_t padding[2];
    uint32_t size;
};

static const uint32_t kFreezeMagic = 0x4C535247;  // "GRSL"
static const uint32_t kFreezeVersion = 1;

struct GresilInstance;

struct FreezeStore {
    pthread_t thread;
    int thread_ok;
    std::atomic<int> running;
    std::atomic<int> request;
    std::atomic<int> generation;
    std::atomic<int> load_done;    // 0 pending, 1 ok, -1 failed
    int job_generation;
    FreezeFileHeader job_header;   // written by the callback before a request
    int32_t loaded_write_ptr;      // written by the worker before load_done
    char data_dir[200];
    char path[256];
    char tmp_path[264];
};

// ---- Instance ----
struct GresilInstance {
    beads::Processor processor;
    beads::Synth synth;

    alignas(16) uint8_t delay_memory[kDelayMemorySize];
    uint16_t reverb_buffer[kReverbBufferSize];

    float value[P_COUNT];
    float smooth[P_COUNT];          // block-smoothed continuous params

    int applied_media;
    int applied_mono;
    int applied_source;
    float fade;                     // engine vs dry-bypass crossfade (1 = engine)
    float drive_gain;               // per-sample smoothed input gain
    float soft_amount;              // per-sample smoothed soft-clip blend

    // Output stage (Filters page): low-shelf biquad (DF2T) + tape limiter.
    float ls_b0, ls_b1, ls_b2, ls_a1, ls_a2;
    float ls_z1[2], ls_z2[2];
    float ls_last_boost, ls_last_freq, ls_last_q;
    float limiter_mix;              // 0 = bypass, 1 = limited (ramped)
    float limiter_pre_gain, limiter_post_gain;

    stmlib::GateFlags previous_seed;
    ClockSync clock;

    FreezeStore store;
    char freeze_id[24];
    int previous_freeze;
    int loading;                    // a load is in flight: engine paused
    int pending_load;               // set by a state restore
    int saved_this_freeze;
    int frozen_samples;
    int trim_request;
};

// ---- Helpers ----
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int clampi(int x, int lo, int hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

static inline int ival(const GresilInstance *inst, int id) {
    return (int)lrintf(inst->value[id]);
}

static inline float db_to_lin(float db) {
    return powf(10.0f, db * 0.05f);
}

// Tape-style soft clip limiter, from Verglas: Chowdhury DSP cubic soft clip
// into an Airwindows IronOxide sine shaper. Stateless, output within +-1.
static inline float tape_soft_clip(float x) {
    float ax = fabsf(x);
    if (ax > 1.5f) {
        x = (x > 0.0f) ? 1.0f : -1.0f;
    } else {
        float cn = clampf(x * 0.6666667f, -1.0f, 1.0f);
        x = (cn - (cn * cn * cn) * 0.3333333f) * 1.5f;
    }
    float bridge = fabsf(x);
    if (bridge > 1.5707963f) bridge = 1.5707963f;
    bridge = sinf(bridge);
    return (x > 0.0f) ? bridge : -bridge;
}

// Low-shelf biquad coefficients (Audio EQ Cookbook), from Verglas.
static void update_low_shelf(GresilInstance *inst, float boost, float freq, float q) {
    if (boost == inst->ls_last_boost && freq == inst->ls_last_freq && q == inst->ls_last_q) return;
    inst->ls_last_boost = boost;
    inst->ls_last_freq = freq;
    inst->ls_last_q = q;
    float A = powf(10.0f, boost * 0.025f);
    float w0 = 6.2831853f * freq / kSampleRate;
    float cosw0 = cosf(w0);
    float alpha = sinf(w0) / (2.0f * q);
    float sqrtA2alpha = 2.0f * sqrtf(A) * alpha;
    float inv_a0 = 1.0f / ((A + 1.0f) + (A - 1.0f) * cosw0 + sqrtA2alpha);
    inst->ls_b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + sqrtA2alpha) * inv_a0;
    inst->ls_b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0) * inv_a0;
    inst->ls_b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - sqrtA2alpha) * inv_a0;
    inst->ls_a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0) * inv_a0;
    inst->ls_a2 = ((A + 1.0f) + (A - 1.0f) * cosw0 - sqrtA2alpha) * inv_a0;
}

// Beads' attenuverter pot law: dead zone around noon, quadratic response.
static inline float attenuverter_law(float a) {
    float v = a * 0.5f;
    float m = fabsf(v) - 0.03f;
    if (m < 0.0f) m = 0.0f;
    return (2.0f / 0.47f) * v * m;
}

static inline bool delay_mode(const GresilInstance *inst) {
    return ival(inst, P_MODE) == 1 && ival(inst, P_SOURCE) == 0;
}

static void ensure_dir(const char *path) {
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

static void store_update_paths(GresilInstance *inst) {
    FreezeStore *s = &inst->store;
    snprintf(s->path, sizeof(s->path), "%s/%s.frz", s->data_dir, inst->freeze_id);
    snprintf(s->tmp_path, sizeof(s->tmp_path), "%s.tmp", s->path);
}

static void store_save(GresilInstance *inst) {
    FreezeStore *s = &inst->store;
    const int gen = s->job_generation;
    ensure_dir(s->data_dir);
    FILE *f = fopen(s->tmp_path, "wb");
    if (!f) return;
    bool ok = fwrite(&s->job_header, sizeof(s->job_header), 1, f) == 1;
    const size_t chunk = 65536;
    for (size_t offset = 0; ok && offset < kDelayMemorySize; offset += chunk) {
        if (s->generation.load(std::memory_order_acquire) != gen ||
            !s->running.load(std::memory_order_acquire)) {
            ok = false;  // buffer changed, or destroy_instance is waiting on us
            break;
        }
        ok = fwrite(inst->delay_memory + offset, 1, chunk, f) == chunk;
    }
    ok = (fclose(f) == 0) && ok;
    if (ok && s->generation.load(std::memory_order_acquire) == gen) {
        rename(s->tmp_path, s->path);
    } else {
        unlink(s->tmp_path);
    }
}

static void store_load(GresilInstance *inst) {
    FreezeStore *s = &inst->store;
    int result = -1;
    FILE *f = fopen(s->path, "rb");
    if (f) {
        FreezeFileHeader h;
        if (fread(&h, sizeof(h), 1, f) == 1 &&
            h.magic == kFreezeMagic && h.version == kFreezeVersion &&
            h.size == kDelayMemorySize &&
            h.media == s->job_header.media && h.mono == s->job_header.mono &&
            fread(inst->delay_memory, 1, kDelayMemorySize, f) == kDelayMemorySize) {
            s->loaded_write_ptr = h.write_ptr;
            result = 1;
        }
        fclose(f);
    }
    s->load_done.store(result, std::memory_order_release);
}

static void *store_worker(void *arg) {
    GresilInstance *inst = (GresilInstance *)arg;
    FreezeStore *s = &inst->store;

    // Created from the audio callback: demote before doing anything else.
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &sp);
#ifdef __linux__
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    CPU_SET(1, &cpus);
    CPU_SET(2, &cpus);
    pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
#endif

    while (s->running.load(std::memory_order_acquire)) {
        int request = s->request.exchange(REQ_NONE, std::memory_order_acquire);
        if (request == REQ_SAVE) {
            store_save(inst);
        } else if (request == REQ_LOAD) {
            store_load(inst);
        } else {
            struct timespec ts = { 0, 20 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
    }
    return NULL;
}

// ---- Engine configuration ----
static void apply_configuration(GresilInstance *inst) {
    int media = ival(inst, P_MEDIA);
    int mono = ival(inst, P_BUFFER);
    int source = ival(inst, P_SOURCE);
    if (media != inst->applied_media || mono != inst->applied_mono) {
        inst->processor.Configure(beads::Processor::Media(media), mono != 0);
        inst->applied_media = media;
        inst->applied_mono = mono;
        // Configure() wipes the buffer but leaves its length at the maximum,
        // so Time would point up to 35 s into silence. Trim it (the hardware's
        // Media+Freeze): Time then spans what has been recorded so far and
        // grows to the full buffer. A freeze restore needs the full length.
        if (!inst->pending_load) inst->processor.Trim();
        // The buffer was cleared: anything on disk no longer matches.
        inst->store.generation.fetch_add(1, std::memory_order_acq_rel);
        inst->saved_this_freeze = 0;
        inst->frozen_samples = 0;
    }
    if (source != inst->applied_source) {
        inst->applied_source = source;
    }
}

static inline bool configuration_pending(const GresilInstance *inst) {
    return ival(inst, P_MEDIA) != inst->applied_media ||
           ival(inst, P_BUFFER) != inst->applied_mono ||
           ival(inst, P_SOURCE) != inst->applied_source;
}

// ARM has no flush-to-zero by default, and -ffast-math only enables it for
// executables (crtfastmath), not for a shared library. Set FPCR.FZ for the
// duration of the block so decaying reverb / filter / feedback tails never
// produce denormals.
struct DenormalGuard {
#if defined(__aarch64__)
    uint64_t saved;
    DenormalGuard() {
        __asm__ __volatile__("mrs %0, fpcr" : "=r"(saved));
        uint64_t fz = saved | (1ull << 24);
        __asm__ __volatile__("msr fpcr, %0" : : "r"(fz));
    }
    ~DenormalGuard() {
        __asm__ __volatile__("msr fpcr, %0" : : "r"(saved));
    }
#endif
};

static void start_load(GresilInstance *inst) {
    FreezeStore *s = &inst->store;
    store_update_paths(inst);
    s->job_header.media = (uint8_t)inst->applied_media;
    s->job_header.mono = (uint8_t)inst->applied_mono;
    s->generation.fetch_add(1, std::memory_order_acq_rel);
    s->load_done.store(0, std::memory_order_release);
    s->request.store(REQ_LOAD, std::memory_order_release);
    inst->loading = 1;
}

static void finish_load(GresilInstance *inst) {
    FreezeStore *s = &inst->store;
    int done = s->load_done.load(std::memory_order_acquire);
    if (!done) return;
    if (done > 0) {
        *inst->processor.mutable_delay_write_ptr() = s->loaded_write_ptr;
        inst->saved_this_freeze = 1;
    } else {
        // Like the hardware: no valid backup means we wake up unfrozen.
        inst->value[P_FREEZE] = 0.0f;
        inst->previous_freeze = 0;
    }
    inst->loading = 0;
}

static void update_freeze_save(GresilInstance *inst, int frames) {
    FreezeStore *s = &inst->store;
    int freeze = ival(inst, P_FREEZE);
    if (!freeze && inst->previous_freeze) {
        // Buffer resumes recording: abort any save in flight.
        s->generation.fetch_add(1, std::memory_order_acq_rel);
        inst->saved_this_freeze = 0;
        inst->frozen_samples = 0;
    }
    inst->previous_freeze = freeze;

    if (!freeze || inst->saved_this_freeze || !s->thread_ok ||
        !ival(inst, P_FREEZE_SAVE) || ival(inst, P_SOURCE) != 0) {
        return;
    }
    inst->frozen_samples += frames;
    if (inst->frozen_samples < (int)(kFreezeSaveDelay * kSampleRate)) return;

    store_update_paths(inst);
    s->job_header.magic = kFreezeMagic;
    s->job_header.version = kFreezeVersion;
    s->job_header.write_ptr = *inst->processor.mutable_delay_write_ptr();
    s->job_header.media = (uint8_t)inst->applied_media;
    s->job_header.mono = (uint8_t)inst->applied_mono;
    s->job_header.size = (uint32_t)kDelayMemorySize;
    s->job_generation = s->generation.load(std::memory_order_acquire);
    s->request.store(REQ_SAVE, std::memory_order_release);
    inst->saved_this_freeze = 1;
}

// ---- Audio ----
static inline void build_parameters(GresilInstance *inst,
                                    beads::Parameters *p,
                                    beads::ModulatedParameter *density,
                                    beads::Processor::MixParameters *m) {
    const float *v = inst->smooth;
    p->time.base = v[P_TIME];
    p->time.attenuverter = attenuverter_law(v[P_TIME_RND]);
    p->time.cv = 0.0f;
    p->time.has_cv = false;
    if (delay_mode(inst)) {
        // Delay: the processor enters its multitap mode when size.base is at
        // the top, and reads the delay filter from the Size attenuverter
        // (bandwidth = (1 + attenuverter) / 2: - low-pass, + high-pass).
        p->size.base = 1.0f;
        p->size.attenuverter = attenuverter_law(v[P_FILTER]);
    } else {
        p->size.base = v[P_SIZE] < kGranularMaxSize ? v[P_SIZE] : kGranularMaxSize;
        p->size.attenuverter = attenuverter_law(v[P_SIZE_RND]);
    }
    p->size.cv = 0.0f;
    p->size.has_cv = false;
    p->shape.base = v[P_SHAPE];
    p->shape.attenuverter = attenuverter_law(v[P_SHAPE_RND]);
    p->shape.cv = 0.0f;
    p->shape.has_cv = false;
    p->pitch.base = inst->value[P_PITCH];
    p->pitch.attenuverter = attenuverter_law(v[P_PITCH_RND]);
    p->pitch.cv = 0.0f;
    p->pitch.has_cv = false;
    p->monophonic_mode = ival(inst, P_MONO_GRAINS) != 0;

    density->base = v[P_DENSITY];
    density->cv = 0.0f;

    m->feedback = clampf(v[P_FEEDBACK] * 1.005f, 0.0f, 1.0f);
    m->dry_wet = clampf(v[P_MIX] * 1.005f, 0.0f, 1.0f);
    m->reverb_amount = clampf(v[P_REVERB] * 1.005f, 0.0f, 1.0f);
    m->output_gate = false;
}

static void gresil_process(void *instance, int16_t *audio_inout, int frames) {
    GresilInstance *inst = (GresilInstance *)instance;
    if (!inst || frames <= 0) return;
    if (frames > kMaxFrames) frames = kMaxFrames;

    // Clock first, so the gate stream covers this block whatever happens next.
    bool clock_gate[kMaxFrames];
    const float division = CLOCK_DIV_BEATS[clampi(ival(inst, P_CLOCK_DIV), 0, 8)];
    const int clock_live = clock_render(&inst->clock, division, clock_gate, frames);

    DenormalGuard denormal_guard;

    // Media / buffer / source changes wipe or swap the engine. Crossfade from
    // the engine to a plain dry bypass, reconfigure while only the bypass is
    // heard, then crossfade back: the dry keeps playing straight through the
    // switch (fading the whole output to silence left an audible hole).
    // Never while a freeze restore is filling the delay banks: Configure()
    // would clear them under the worker's feet. The change waits for the load.
    bool reconfigure = configuration_pending(inst) && !inst->loading;
    const bool configure_now = reconfigure && inst->fade <= 0.0f;
    if (configure_now) {
        // Clearing the 768 KB buffer is the heaviest thing Gresil ever does on
        // the callback, so the engine sits this block out; the bypass plays.
        apply_configuration(inst);
        reconfigure = false;
    }

    if (inst->pending_load && !reconfigure) {
        inst->pending_load = 0;
        if (inst->store.thread_ok) start_load(inst);
        else inst->value[P_FREEZE] = 0.0f;
    }
    if (inst->loading) {
        finish_load(inst);
        if (inst->loading) {
            // The worker is filling the delay banks: the engine must not run.
            // Only happens while a set loads; stay muted, fade in afterwards.
            memset(audio_inout, 0, (size_t)frames * 2 * sizeof(int16_t));
            inst->fade = 0.0f;
            return;
        }
    }

    if (inst->trim_request) {
        inst->trim_request = 0;
        inst->processor.Trim();
        inst->value[P_FREEZE] = 0.0f;
        inst->store.generation.fetch_add(1, std::memory_order_acq_rel);
    }
    update_freeze_save(inst, frames);

    // Input conversion with drive. The tape media soft-clip like the
    // hardware's manual-gain path (its auto-gain default never clips, so at
    // Drive 0 nothing does and the dry stays clean); the digital media clip
    // hard. Dirty Tape is fully soft-clipped from +3 dB, Clean Tape blends in
    // up to +12 dB. Gain and
    // soft-clip blend are smoothed per sample (Drive moves in 0.5 dB steps,
    // which clicked when applied once per block), and both paths are always
    // computed so the blend never switches branch mid-signal.
    const int media = inst->applied_media;
    const float drive_target = powf(10.0f, inst->value[P_DRIVE] * 0.05f);
    float soft_target = 0.0f;
    if (media == beads::Processor::MEDIA_DIRTY_TAPE) {
        soft_target = clampf(inst->value[P_DRIVE] / 3.0f, 0.0f, 1.0f);
    } else if (media == beads::Processor::MEDIA_CLEAN_TAPE) {
        soft_target = clampf(inst->value[P_DRIVE] / 12.0f, 0.0f, 1.0f);
    }
    FloatFrame in[kMaxFrames];
    FloatFrame out[kMaxFrames];
    float drive = inst->drive_gain;
    float soft_amount = inst->soft_amount;
    for (int i = 0; i < frames; ++i) {
        drive += kDriveSmoothing * (drive_target - drive);
        soft_amount += kDriveSmoothing * (soft_target - soft_amount);
        float l = audio_inout[2 * i] / 32768.0f * drive;
        float r = audio_inout[2 * i + 1] / 32768.0f * drive;
        const float bias = 0.05f;
        float sl = (1.0f - bias) * stmlib::SoftClip(l + bias) - bias;
        float sr = (1.0f - bias) * stmlib::SoftClip(r + bias) - bias;
        in[i].l = stmlib::Crossfade(clampf(l, -1.0f, 1.0f), sl, soft_amount);
        in[i].r = stmlib::Crossfade(clampf(r, -1.0f, 1.0f), sr, soft_amount);
    }
    inst->drive_gain = drive;
    inst->soft_amount = soft_amount;

    // Seed stream: Beads treats a clocked SEED input with the button locked
    // as a clock to divide (granular) or to lock the delay time to.
    stmlib::GateFlags seed[kMaxFrames];
    const bool use_clock = ival(inst, P_SEED) == 1 && clock_live;
    if (use_clock) {
        for (int i = 0; i < frames; ++i) {
            inst->previous_seed = seed[i] =
                stmlib::ExtractGateFlags(inst->previous_seed, clock_gate[i]);
        }
    } else {
        inst->previous_seed = stmlib::GATE_FLAG_LOW;
    }

    inst->processor.set_clean_dry(ival(inst, P_DRY) == 0);
    const bool synth_mode = inst->applied_source == 1;
    bool freeze[kSubBlock];
    const bool frozen = ival(inst, P_FREEZE) != 0;
    for (int i = 0; i < kSubBlock; ++i) freeze[i] = frozen;

    if (configure_now) {
        memset(out, 0, sizeof(FloatFrame) * (size_t)frames);
    }
    for (int offset = 0; !configure_now && offset < frames; offset += kSubBlock) {
        int n = frames - offset < kSubBlock ? frames - offset : kSubBlock;
        n &= ~1;  // the 2x decimating media need even block sizes
        if (n <= 0) {
            out[offset] = in[offset];
            continue;
        }

        for (int k = 0; k < P_COUNT; ++k) {
            if (PARAMS[k].type == T_FLOAT) {
                inst->smooth[k] += 0.12f * (inst->value[k] - inst->smooth[k]);
            }
        }
        beads::Parameters p;
        beads::ModulatedParameter density;
        beads::Processor::MixParameters m;
        build_parameters(inst, &p, &density, &m);
        // Both engines get the cutoffs so a source switch carries them over.
        inst->processor.mutable_wet_filter()->set_cutoffs(
            inst->smooth[P_FILTER_HP], inst->smooth[P_FILTER_LP], kSampleRate);
        inst->synth.mutable_wet_filter()->set_cutoffs(
            inst->smooth[P_FILTER_HP], inst->smooth[P_FILTER_LP], kSampleRate);

        const stmlib::GateFlags *seed_in = use_clock ? &seed[offset] : NULL;
        if (synth_mode) {
            inst->synth.Render(
                beads::SEED_BUTTON_STATE_LOCKED, seed_in, freeze,
                beads::Synth::Resolution(media), density, p, m,
                &out[offset], (size_t)n);
        } else {
            inst->processor.Process(
                beads::SEED_BUTTON_STATE_LOCKED, seed_in, freeze,
                density, p, m, &in[offset], &out[offset], (size_t)n);
        }
    }

    // Engine <-> dry bypass crossfade (see the reconfigure note above). The
    // bypass is the input at the dry level of the current Mix (Beads' law,
    // clean-dry gain), so at Mix 0 a switch is inaudible.
    {
        const float m = clampf(inst->smooth[P_MIX] * 1.005f, 0.0f, 1.0f);
        const float dry_gain = (1.0f - m) * (1.0f + m) *
            (ival(inst, P_DRY) == 0 ? 1.0f : 0.75f);
        const float fade_step = (reconfigure || configure_now ? -1.0f : 1.0f) /
            (kFadeTime * kSampleRate);
        for (int i = 0; i < frames; ++i) {
            const float g = inst->fade;
            inst->fade = clampf(g + fade_step, 0.0f, 1.0f);
            out[i].l = out[i].l * g + in[i].l * dry_gain * (1.0f - g);
            out[i].r = out[i].r * g + in[i].r * dry_gain * (1.0f - g);
        }
    }

    // Output stage. Both run all the time (no bypass branch to click on):
    // the shelf is unity at 0 dB, the limiter crossfades in and out.
    update_low_shelf(inst, inst->smooth[P_LOW_BOOST], inst->smooth[P_LOW_FREQ],
                     inst->smooth[P_LOW_Q]);
    const float pre_target = db_to_lin(inst->smooth[P_LIMITER_PRE]);
    const float post_target = db_to_lin(inst->smooth[P_LIMITER_POST]);
    const float pre_step = (pre_target - inst->limiter_pre_gain) / frames;
    const float post_step = (post_target - inst->limiter_post_gain) / frames;
    const float mix_target = ival(inst, P_LIMITER_ON) ? 1.0f : 0.0f;
    const float mix_step = 1.0f / (kFadeTime * kSampleRate);
    for (int i = 0; i < frames; ++i) {
        float x[2] = { out[i].l, out[i].r };
        inst->limiter_pre_gain += pre_step;
        inst->limiter_post_gain += post_step;
        float lm = inst->limiter_mix;
        lm = lm < mix_target ? fminf(lm + mix_step, mix_target) : fmaxf(lm - mix_step, mix_target);
        inst->limiter_mix = lm;
        for (int c = 0; c < 2; ++c) {
            float y = inst->ls_b0 * x[c] + inst->ls_z1[c];
            inst->ls_z1[c] = inst->ls_b1 * x[c] - inst->ls_a1 * y + inst->ls_z2[c];
            inst->ls_z2[c] = inst->ls_b2 * x[c] - inst->ls_a2 * y;
            if (lm > 0.0f) {
                float limited = tape_soft_clip(y * inst->limiter_pre_gain) * inst->limiter_post_gain;
                y += (limited - y) * lm;
            }
            x[c] = y;
        }
        out[i].l = x[0];
        out[i].r = x[1];
    }

    for (int i = 0; i < frames; ++i) {
        float l = clampf(out[i].l, -1.0f, 1.0f);
        float r = clampf(out[i].r, -1.0f, 1.0f);
        audio_inout[2 * i] = (int16_t)lrintf(l * 32767.0f);
        audio_inout[2 * i + 1] = (int16_t)lrintf(r * 32767.0f);
    }
}

// ---- MIDI (clock only) ----
static void gresil_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)source;
    GresilInstance *inst = (GresilInstance *)instance;
    if (!inst || !msg || len < 1) return;
    uint8_t status = msg[0];
    if (status == 0xF8 || status == 0xFA || status == 0xFB || status == 0xFC) {
        clock_on_realtime(&inst->clock, status);
    }
}

// ---- Lifecycle ----
static void *gresil_create(const char *module_dir, const char *config_json) {
    (void)module_dir;
    (void)config_json;
    GresilInstance *inst = (GresilInstance *)calloc(1, sizeof(GresilInstance));
    if (!inst) return NULL;

    uint8_t *banks[kNumBanks];
    for (size_t i = 0; i < kNumBanks; ++i) banks[i] = inst->delay_memory + i * kBankSize;
    inst->processor.Init(banks, inst->reverb_buffer);
    inst->synth.Init(inst->reverb_buffer);

    for (int k = 0; k < P_COUNT; ++k) {
        inst->value[k] = PARAMS[k].def;
        inst->smooth[k] = PARAMS[k].def;
    }
    inst->applied_media = -1;
    inst->applied_mono = -1;
    inst->applied_source = 0;
    inst->fade = 1.0f;
    inst->drive_gain = 1.0f;
    inst->soft_amount = 0.0f;
    inst->ls_last_boost = -999.0f;  // force the first coefficient update
    update_low_shelf(inst, PARAMS[P_LOW_BOOST].def, PARAMS[P_LOW_FREQ].def, PARAMS[P_LOW_Q].def);
    inst->limiter_pre_gain = 1.0f;
    inst->limiter_post_gain = 1.0f;
    inst->limiter_mix = 0.0f;
    inst->previous_seed = stmlib::GATE_FLAG_LOW;
    clock_init(&inst->clock);
    apply_configuration(inst);

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t a = (uint32_t)ts.tv_nsec ^ (uint32_t)((uintptr_t)inst >> 4);
    uint32_t b = (uint32_t)ts.tv_sec ^ (uint32_t)getpid() * 2654435761u;
    snprintf(inst->freeze_id, sizeof(inst->freeze_id), "%08x%08x", a, b);

    FreezeStore *s = &inst->store;
    const char *dir = getenv("GRESIL_DATA_DIR");
    snprintf(s->data_dir, sizeof(s->data_dir), "%s", dir && *dir ? dir : kDefaultDataDir);
    store_update_paths(inst);
    s->running.store(1);
    s->request.store(REQ_NONE);
    s->generation.store(0);
    s->load_done.store(0);
    s->thread_ok = pthread_create(&s->thread, NULL, store_worker, inst) == 0;

    return inst;
}

static void gresil_destroy(void *instance) {
    GresilInstance *inst = (GresilInstance *)instance;
    if (!inst) return;
    if (inst->store.thread_ok) {
        inst->store.running.store(0, std::memory_order_release);
        pthread_join(inst->store.thread, NULL);
    }
    free(inst);
}

// ---- Parameters ----
static int find_param(const char *key) {
    for (int k = 0; k < P_COUNT; ++k) {
        if (strcmp(key, PARAMS[k].key) == 0) return k;
    }
    return -1;
}

static void set_value(GresilInstance *inst, int id, const char *val) {
    const ParamDef &d = PARAMS[id];
    if (d.type == T_TRIGGER) {
        if (strcmp(val, "On") == 0 || atoi(val) != 0) inst->trim_request = 1;
        return;
    }
    if (d.type == T_ENUM) {
        for (int i = 0; i < d.num_options; ++i) {
            if (strcmp(val, d.options[i]) == 0) {
                inst->value[id] = (float)i;
                return;
            }
        }
        inst->value[id] = (float)clampi(atoi(val), 0, d.num_options - 1);
        return;
    }
    float v = clampf((float)atof(val), d.min, d.max);
    if (d.type == T_INT) v = (float)lrintf(v);
    inst->value[id] = v;
}

// Raw value, as used by the menu and the state snapshot (no units).
static int format_raw(const GresilInstance *inst, int id, char *buf, int buf_len) {
    const ParamDef &d = PARAMS[id];
    switch (d.type) {
        case T_TRIGGER:
            return snprintf(buf, buf_len, "Off");
        case T_ENUM:
            return snprintf(buf, buf_len, "%s",
                            d.options[clampi(ival(inst, id), 0, d.num_options - 1)]);
        case T_INT:
            return snprintf(buf, buf_len, "%d", ival(inst, id));
        default:
            return snprintf(buf, buf_len, "%.4f", inst->value[id]);
    }
}

// State: every user-settable key (Trim excluded) plus the freeze file id.
// Values are quoted raw strings; the parser is key-based and whitespace
// tolerant because the host re-emits the JSON pretty-printed.
static int json_get_str(const char *json, const char *key, char *out, int out_len) {
    char search[48];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += strlen(search);
    while (*p == ':' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if (*p != '"') return -1;
    p++;
    const char *end = strchr(p, '"');
    if (!end) return -1;
    int len = (int)(end - p);
    if (len >= out_len) len = out_len - 1;
    memcpy(out, p, (size_t)len);
    out[len] = '\0';
    return 0;
}

static void gresil_set_param(void *instance, const char *key, const char *val);

static void restore_state(GresilInstance *inst, const char *json) {
    char vb[64];
    for (int k = 0; k < P_COUNT; ++k) {
        if (PARAMS[k].type == T_TRIGGER) continue;
        if (json_get_str(json, PARAMS[k].key, vb, sizeof(vb)) == 0) {
            gresil_set_param(inst, PARAMS[k].key, vb);
        }
    }
    // States saved before Mode/Filter existed: delay mode was Size at max and
    // the Size attenuverter was the delay filter.
    if (json_get_str(json, "size", vb, sizeof(vb)) == 0 &&
        json_get_str(json, "mode", vb, sizeof(vb)) != 0) {
        const bool was_delay = inst->value[P_SIZE] > 0.99f;
        inst->value[P_MODE] = was_delay ? 1.0f : 0.0f;
        if (was_delay && json_get_str(json, "filter", vb, sizeof(vb)) != 0) {
            inst->value[P_FILTER] = inst->value[P_SIZE_RND];
            inst->smooth[P_FILTER] = inst->value[P_FILTER];
            inst->value[P_SIZE_RND] = 0.0f;
        }
    }
    if (json_get_str(json, "freeze_id", vb, sizeof(vb)) == 0) {
        bool valid = vb[0] != '\0';
        for (const char *c = vb; *c; ++c) {
            if (!((*c >= '0' && *c <= '9') || (*c >= 'a' && *c <= 'f'))) valid = false;
        }
        if (valid && strlen(vb) < sizeof(inst->freeze_id)) {
            snprintf(inst->freeze_id, sizeof(inst->freeze_id), "%s", vb);
        }
    }
    // A set saved while frozen gets its buffer back from disk. Without a
    // backup to load (saving off, or the synth source) it wakes up unfrozen,
    // like the hardware with autosave disabled.
    if (ival(inst, P_FREEZE)) {
        if (ival(inst, P_FREEZE_SAVE) && ival(inst, P_SOURCE) == 0) {
            inst->pending_load = 1;
            inst->previous_freeze = 1;
        } else {
            inst->value[P_FREEZE] = 0.0f;
        }
    }
}

static void gresil_set_param(void *instance, const char *key, const char *val) {
    GresilInstance *inst = (GresilInstance *)instance;
    if (!inst || !key || !val) return;

    if (strcmp(key, "state") == 0) {
        restore_state(inst, val);
        return;
    }
    int id = find_param(key);
    if (id >= 0) set_value(inst, id, val);
}

static int gresil_get_param(void *instance, const char *key, char *buf, int buf_len) {
    GresilInstance *inst = (GresilInstance *)instance;
    if (!inst || !key || !buf || buf_len < 1) return -1;

    if (strcmp(key, "name") == 0) return snprintf(buf, buf_len, "Gresil");
    // Metadata lives in module.json (single source of truth).
    if (strcmp(key, "chain_params") == 0) return -1;
    if (strcmp(key, "ui_hierarchy") == 0) return -1;

    if (strcmp(key, "state") == 0) {
        int n = 0;
        if (buf_len < 2) return -1;
        buf[n++] = '{';
        for (int k = 0; k < P_COUNT; ++k) {
            if (PARAMS[k].type == T_TRIGGER) continue;
            char vb[32];
            format_raw(inst, k, vb, sizeof(vb));
            int w = snprintf(buf + n, (size_t)(buf_len - n), "%s\"%s\":\"%s\"",
                             n > 1 ? "," : "", PARAMS[k].key, vb);
            if (w < 0 || w >= buf_len - n) return -1;
            n += w;
        }
        int w = snprintf(buf + n, (size_t)(buf_len - n), ",\"freeze_id\":\"%s\"}",
                         inst->freeze_id);
        if (w < 0 || w >= buf_len - n) return -1;
        return n + w;
    }
    int id = find_param(key);
    if (id >= 0) return format_raw(inst, id, buf, buf_len);
    return -1;
}

// ---- Static API struct ----
static audio_fx_api_v2_t g_api = {
    .api_version = 2,
    .create_instance = gresil_create,
    .destroy_instance = gresil_destroy,
    .process_block = gresil_process,
    .set_param = gresil_set_param,
    .get_param = gresil_get_param,
    .on_midi = gresil_on_midi,
};

extern "C" {

// chain_host resolves on_midi through dlsym, not through the struct.
__attribute__((visibility("default")))
void move_audio_fx_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    gresil_on_midi(instance, msg, len, source);
}

__attribute__((visibility("default")))
audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    return &g_api;
}

}  // extern "C"
