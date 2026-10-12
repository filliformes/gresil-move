// Native tests for the Gresil wrapper. The wrapper is included directly so
// its internals (clock PLL, freeze store) can be checked.
//
// Build + run: bash scripts/test.sh

// Standard headers first, then open up the Beads classes so the tests can
// inspect grains directly.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>
#define private public
#define protected public
#include "../src/gresil_move.cpp"
#undef private
#undef protected

#include <chrono>
#include <string>
#include <vector>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...) do { \
    ++g_checks; \
    if (!(cond)) { \
        ++g_failures; \
        printf("FAIL %s:%d: ", __FILE__, __LINE__); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

static void test_log(const char *msg) { (void)msg; }

static host_api_v1_t g_test_host;

static audio_fx_api_v2_t *api() {
    static audio_fx_api_v2_t *a = NULL;
    if (!a) {
        memset(&g_test_host, 0, sizeof(g_test_host));
        g_test_host.api_version = 1;
        g_test_host.sample_rate = 44100;
        g_test_host.frames_per_block = 128;
        g_test_host.log = test_log;
        a = move_audio_fx_init_v2(&g_test_host);
    }
    return a;
}

static std::string get(void *inst, const char *key) {
    char buf[4096];
    int n = api()->get_param(inst, key, buf, sizeof(buf));
    if (n < 0) return "<none>";
    return std::string(buf, (size_t)n);
}

static void set(void *inst, const char *key, const char *val) {
    api()->set_param(inst, key, val);
}

// Stereo test signal: two detuned sines with a slow amplitude wobble.
struct Source {
    double t = 0;
    void fill(int16_t *buf, int frames, float amp = 0.5f) {
        for (int i = 0; i < frames; ++i) {
            double env = 0.75 + 0.25 * sin(t * 2 * M_PI * 1.3);
            buf[2 * i] = (int16_t)(amp * env * sin(t * 2 * M_PI * 220.0) * 32767);
            buf[2 * i + 1] = (int16_t)(amp * env * sin(t * 2 * M_PI * 331.0) * 32767);
            t += 1.0 / 44100.0;
        }
    }
};

struct Stats {
    double sum_sq = 0;
    int peak = 0;
    long n = 0;
    void add(const int16_t *buf, int frames) {
        for (int i = 0; i < 2 * frames; ++i) {
            sum_sq += (double)buf[i] * buf[i];
            if (abs(buf[i]) > peak) peak = abs(buf[i]);
            ++n;
        }
    }
    double rms() const { return n ? sqrt(sum_sq / n) / 32768.0 : 0.0; }
};

static void run(void *inst, Source *src, float seconds, Stats *stats = NULL,
                double *max_us = NULL, bool silent = false) {
    int blocks = (int)(seconds * 44100 / 128);
    int16_t buf[256];
    for (int b = 0; b < blocks; ++b) {
        if (silent) memset(buf, 0, sizeof(buf));
        else src->fill(buf, 128);
        auto t0 = std::chrono::steady_clock::now();
        api()->process_block(inst, buf, 128);
        auto t1 = std::chrono::steady_clock::now();
        if (max_us) {
            double us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            if (us > *max_us) *max_us = us;
        }
        if (stats) stats->add(buf, 128);
    }
}

static void test_media_and_modes() {
    const char *media[] = { "Dirty Tape", "Clean Tape", "Vintage", "Digital" };
    const char *buffers[] = { "Stereo", "Mono" };
    double worst = 0;
    for (int m = 0; m < 4; ++m) {
        for (int b = 0; b < 2; ++b) {
            for (int delay = 0; delay < 2; ++delay) {
                void *inst = api()->create_instance("", NULL);
                set(inst, "media", media[m]);
                set(inst, "buffer", buffers[b]);
                set(inst, "dry_wet", "1");
                set(inst, "feedback", "0.5");
                set(inst, "reverb", "0.4");
                set(inst, "size", "0.6");
                set(inst, "mode", delay ? "Delay" : "Granular");
                // Time picks a position across the whole (up to 17 s) buffer:
                // stay near the write head so the short test hears grains.
                set(inst, "time", "0.03");
                set(inst, "density", "0.85");
                set(inst, "time_rnd", "0.5");
                set(inst, "pitch_rnd", "-0.4");
                Source src;
                Stats st;
                double max_us = 0;
                run(inst, &src, 1.5f);                 // fill, settle the delay transition
                run(inst, &src, 2.0f, &st, &max_us);
                CHECK(st.rms() > 0.01, "%s/%s/%s: wet output silent (rms %f)",
                      media[m], buffers[b], delay ? "delay" : "granular", st.rms());
                CHECK(st.rms() < 1.0, "%s: rms out of range", media[m]);
                if (max_us > worst) worst = max_us;
                printf("  %-10s %-6s %-8s rms %.3f peak %5d  worst block %6.1f us\n",
                       media[m], buffers[b], delay ? "delay" : "granular",
                       st.rms(), st.peak, max_us);
                api()->destroy_instance(inst);
            }
        }
    }
    printf("  worst block (native): %.1f us of a 2902 us budget\n", worst);
}

static void test_clean_dry_is_transparent() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "0");
    set(inst, "reverb", "0");
    Source a, b;
    int16_t buf[256], ref[256];
    run(inst, &a, 0.5f);
    b = a;
    int max_diff = 0;
    for (int i = 0; i < 200; ++i) {
        a.fill(buf, 128);
        b.fill(ref, 128);
        api()->process_block(inst, buf, 128);
        for (int k = 0; k < 256; ++k) {
            int d = abs(buf[k] - ref[k]);
            if (d > max_diff) max_diff = d;
        }
    }
    CHECK(max_diff <= 2, "Mix 0 with clean dry should pass the input (max diff %d)", max_diff);
    api()->destroy_instance(inst);
}

static void test_synth_source() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "source", "Synth");
    set(inst, "dry_wet", "0.5");
    Source src;
    Stats st;
    run(inst, &src, 1.0f, NULL, NULL, true);
    run(inst, &src, 1.0f, &st, NULL, true);
    CHECK(st.rms() > 0.01, "synth source should play with no input (rms %f)", st.rms());
    set(inst, "_level", "Beads");
    CHECK(get(inst, "knob_6_name") == "Bank", "synth: knob 6 should read Bank, got %s",
          get(inst, "knob_6_name").c_str());
    api()->destroy_instance(inst);
}

static void test_clock_pll() {
    // 120 BPM: a tick every 918.75 samples, delivered at block boundaries.
    ClockSync c;
    clock_init(&c);
    clock_on_realtime(&c, 0xFA);
    bool gate[128];
    double next_tick = 0;
    long sample = 0;
    bool prev = false;
    std::vector<long> edges;
    for (int b = 0; b < 4000; ++b) {
        while (next_tick <= sample) {
            clock_on_realtime(&c, 0xF8);
            next_tick += 44100.0 * 0.5 / 24.0;
        }
        clock_render(&c, 1.0f, gate, 128);
        for (int i = 0; i < 128; ++i) {
            if (gate[i] && !prev) edges.push_back(sample + i);
            prev = gate[i];
        }
        sample += 128;
    }
    CHECK(edges.size() > 20, "clock: expected quarter-note edges, got %zu", edges.size());
    double worst = 0;
    for (size_t i = 8; i + 1 < edges.size(); ++i) {
        double err = fabs((double)(edges[i + 1] - edges[i]) - 22050.0);
        if (err > worst) worst = err;
    }
    printf("  clock: %zu edges, worst interval error after lock %.1f samples\n",
           edges.size(), worst);
    CHECK(worst < 44.1, "clock jitter %.1f samples exceeds Beads' 1ms tolerance", worst);

    clock_on_realtime(&c, 0xFC);
    CHECK(!clock_is_live(&c), "clock: Stop should stop the gate stream");
}

static void test_clock_seed_runs() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "seed", "Clock");
    set(inst, "density", "0.2");
    set(inst, "dry_wet", "1");
    uint8_t start = 0xFA, tick = 0xF8;
    move_audio_fx_on_midi(inst, &start, 1, 0);
    Source src;
    Stats st;
    int16_t buf[256];
    double next_tick = 0;
    long sample = 0;
    for (int b = 0; b < 1500; ++b) {
        while (next_tick <= sample) {
            move_audio_fx_on_midi(inst, &tick, 1, 0);
            next_tick += 44100.0 * 0.5 / 24.0;
        }
        src.fill(buf, 128);
        api()->process_block(inst, buf, 128);
        if (b > 300) st.add(buf, 128);
        sample += 128;
    }
    CHECK(st.rms() > 0.005, "clocked grains should produce output (rms %f)", st.rms());
    // Delay mode locked to the clock.
    set(inst, "mode", "Delay");
    for (int b = 0; b < 1500; ++b) {
        while (next_tick <= sample) {
            move_audio_fx_on_midi(inst, &tick, 1, 0);
            next_tick += 44100.0 * 0.5 / 24.0;
        }
        src.fill(buf, 128);
        api()->process_block(inst, buf, 128);
        sample += 128;
    }
    set(inst, "_level", "Beads");
    CHECK(get(inst, "knob_4_name") == "Ratio", "clocked delay: knob 4 should read Ratio, got %s",
          get(inst, "knob_4_name").c_str());
    api()->destroy_instance(inst);
}

// The user-facing complaint: grains were scarce. Count overlapping grains at
// the default settings on a continuous input.
static void test_default_grain_density() {
    void *inst = api()->create_instance("", NULL);
    GresilInstance *g = (GresilInstance *)inst;
    Source src;
    int16_t buf[256];
    long active = 0, n = 0;
    for (int b = 0; b < 1500; ++b) {
        src.fill(buf, 128);
        api()->process_block(inst, buf, 128);
        if (b > 300) {
            active += g->processor.processor_digital_.granular_player_.CountActiveGrains();
            ++n;
        }
    }
    double avg = (double)active / n;
    printf("  defaults: %.2f grains overlapping on average\n", avg);
    CHECK(avg > 2.0, "defaults should give a continuous texture (avg %.2f grains)", avg);
    api()->destroy_instance(inst);
}

// Random knobs: grain start positions and pitches must spread when the
// attenuverters are up, and stay put when they are at noon.
struct Spread { double min_delay = 1e30, max_delay = -1e30; double min_speed = 1e30, max_speed = -1e30; int grains = 0; };

static Spread measure_spread(const char *time_rnd, const char *pitch_rnd) {
    void *inst = api()->create_instance("", NULL);
    GresilInstance *g = (GresilInstance *)inst;
    set(inst, "time_rnd", time_rnd);
    set(inst, "pitch_rnd", pitch_rnd);
    Source src;
    int16_t buf[256];
    Spread sp;
    auto &player = g->processor.processor_digital_.granular_player_;
    std::vector<int32_t> seen(beads::DelayGranularPlayer::NUM_GRAINS, -1);
    for (int b = 0; b < 2500; ++b) {
        src.fill(buf, 128);
        api()->process_block(inst, buf, 128);
        if (b < 1200) continue;  // let the trimmed buffer grow first
        for (int i = 0; i < beads::DelayGranularPlayer::NUM_GRAINS; ++i) {
            const beads::DelayGrain &grain = player.grain_[i];
            if (grain.done() || grain.initial_delay_ == seen[i]) continue;
            seen[i] = grain.initial_delay_;
            double d = grain.initial_delay_ / 1024.0;
            double v = fabs(grain.speed_ / 1024.0);
            sp.min_delay = std::min(sp.min_delay, d);
            sp.max_delay = std::max(sp.max_delay, d);
            sp.min_speed = std::min(sp.min_speed, v);
            sp.max_speed = std::max(sp.max_speed, v);
            ++sp.grains;
        }
    }
    api()->destroy_instance(inst);
    return sp;
}

static void test_random_knobs() {
    Spread fixed = measure_spread("0", "0");
    Spread rnd = measure_spread("1", "1");
    Spread jumps = measure_spread("-1", "-1");
    printf("  Rnd 0:  %d grains, start %.0f..%.0f samples, speed %.2f..%.2f\n",
           fixed.grains, fixed.min_delay, fixed.max_delay, fixed.min_speed, fixed.max_speed);
    printf("  Rnd +1: %d grains, start %.0f..%.0f samples, speed %.2f..%.2f\n",
           rnd.grains, rnd.min_delay, rnd.max_delay, rnd.min_speed, rnd.max_speed);
    printf("  Rnd -1: %d grains, start %.0f..%.0f samples, speed %.2f..%.2f\n",
           jumps.grains, jumps.min_delay, jumps.max_delay, jumps.min_speed, jumps.max_speed);
    CHECK(fixed.grains > 10 && rnd.grains > 10 && jumps.grains > 10, "grains should fire");
    CHECK(fixed.max_speed - fixed.min_speed < 0.01, "Pitch Rnd 0: all grains at the same pitch");
    CHECK(rnd.max_speed / rnd.min_speed > 4.0, "Pitch Rnd +1: grains should spread over octaves");
    CHECK(jumps.max_speed / jumps.min_speed > 2.0, "Pitch Rnd -1: occasional random jumps");
    double fixed_span = fixed.max_delay - fixed.min_delay;
    double rnd_span = rnd.max_delay - rnd.min_delay;
    CHECK(rnd_span > 10.0 * (fixed_span + 1000.0), "Time Rnd +1: start positions should spread (%.0f vs %.0f)",
          rnd_span, fixed_span);
}

// Max sample-to-sample step of the output, to catch clicks.
static int max_step(void *inst, Source *src, int blocks, void (*mid)(void *), int at_block) {
    int16_t buf[256];
    int prev_l = 0, worst = 0;
    bool first = true;
    for (int b = 0; b < blocks; ++b) {
        if (b == at_block && mid) mid(inst);
        src->fill(buf, 128, 0.05f);
        api()->process_block(inst, buf, 128);
        for (int i = 0; i < 128; ++i) {
            if (!first) worst = std::max(worst, abs(buf[2 * i] - prev_l));
            prev_l = buf[2 * i];
            first = false;
        }
    }
    return worst;
}

static void drive_up(void *inst) { set(inst, "drive", "24"); }
static void media_dirty(void *inst) { set(inst, "media", "Dirty Tape"); }

static void test_click_free_controls() {
    // Mix 0: the output is the (driven) input, a 220 Hz sine at -26 dBFS.
    void *inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "0");
    Source src;
    max_step(inst, &src, 50, NULL, 0);
    int worst = max_step(inst, &src, 200, drive_up, 100);
    // +24 dB of a 0.05 sine: natural slope ~ 2*pi*220/44100 * 0.79 * 32767 = 812.
    printf("  drive 0 -> 24 dB: max step %d (natural slope at the final level ~812)\n", worst);
    CHECK(worst < 1000, "Drive jump should be smoothed (max step %d)", worst);
    api()->destroy_instance(inst);

    inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "0");
    max_step(inst, &src, 50, NULL, 0);
    worst = max_step(inst, &src, 200, media_dirty, 100);
    printf("  media switch: max step %d (natural slope ~52)\n", worst);
    CHECK(worst < 120, "Media change should fade, not click (max step %d)", worst);
    api()->destroy_instance(inst);
}

// After a media change the buffer is empty: grains must come back as soon as
// audio is recorded, not after the whole (17-35 s) buffer has filled.
static void test_audible_after_media_change() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "media", "Dirty Tape");
    set(inst, "buffer", "Mono");
    set(inst, "time", "0.5");
    set(inst, "dry_wet", "1");
    Source src;
    Stats st;
    run(inst, &src, 1.0f);
    run(inst, &src, 1.0f, &st);
    printf("  Dirty Tape mono, Time 50%%, 1-2 s after the switch: rms %.3f\n", st.rms());
    CHECK(st.rms() > 0.02, "grains should play recent audio after a media change (rms %f)", st.rms());
    api()->destroy_instance(inst);
}

// Band energies of a bass-heavy test signal: < ~200 Hz vs > ~2 kHz.
struct Tilt { double lo = 0, hi = 0; double lp = 0, hp = 0;
    void add(double x) { lp += 0.028 * (x - lp); double h = x - hp; hp += 0.25 * (x - hp); lo += lp * lp; hi += h * h; } };

static double measure_tilt(void (*setup)(void *)) {
    void *inst = api()->create_instance("", NULL);
    set(inst, "dry_wet", "1");
    setup(inst);
    int16_t buf[256];
    double t = 0, ph = 0;
    Tilt in, out;
    for (int b = 0; b < 3000; ++b) {
        for (int i = 0; i < 128; ++i) {
            ph += 55.0 / 44100.0; ph -= floor(ph);
            double v = 0.3 * (2 * ph - 1) + 0.1 * sin(t * 2 * M_PI * 880.0);
            t += 1.0 / 44100.0;
            buf[2 * i] = buf[2 * i + 1] = (int16_t)(v * 20000);
        }
        if (b > 1500) for (int i = 0; i < 128; ++i) in.add(buf[2 * i] / 32768.0);
        api()->process_block(inst, buf, 128);
        if (b > 1500) for (int i = 0; i < 128; ++i) out.add(buf[2 * i] / 32768.0);
    }
    api()->destroy_instance(inst);
    return 10 * log10(out.lo / in.lo) - 10 * log10(out.hi / in.hi);
}

static void setup_granular_size_rnd(void *inst) { set(inst, "size_rnd", "1"); }
static void setup_delay_flat(void *inst) { set(inst, "mode", "Delay"); set(inst, "density", "0.7"); }
static void setup_delay_hp(void *inst) { setup_delay_flat(inst); set(inst, "filter", "1"); }
static void setup_delay_lp(void *inst) { setup_delay_flat(inst); set(inst, "filter", "-1"); }

// The reported "horribly high-passed grains": Size Rnd used to be the delay
// filter once Size reached the top. Now it only ever randomizes grain size.
static void test_mode_and_filter() {
    double granular = measure_tilt(setup_granular_size_rnd);
    double flat = measure_tilt(setup_delay_flat);
    double hp = measure_tilt(setup_delay_hp);
    double lp = measure_tilt(setup_delay_lp);
    printf("  low/high tilt: granular+SizeRnd %+.1f dB, delay %+.1f dB, delay HP %+.1f dB, delay LP %+.1f dB\n",
           granular, flat, hp, lp);
    CHECK(fabs(granular) < 3.0, "Size Rnd must not filter (tilt %.1f dB)", granular);
    CHECK(fabs(flat) < 3.0, "delay with Filter off should be flat (tilt %.1f dB)", flat);
    CHECK(hp < flat - 12.0, "Filter + should high-pass (tilt %.1f dB)", hp);
    CHECK(lp > flat + 12.0, "Filter - should low-pass (tilt %.1f dB)", lp);
}

// Sets saved before Mode existed: Size at max meant delay mode, and the Size
// attenuverter was the filter.
static void test_mode_migration() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "state", "{\"size\": \"1.0000\", \"size_rnd\": \"1.0000\", \"density\": \"0.5000\"}");
    CHECK(get(inst, "mode") == "Delay", "old Size-at-max state should restore as Delay");
    CHECK(get(inst, "filter") == "1.0000", "old Size Rnd should become the filter: %s", get(inst, "filter").c_str());
    CHECK(get(inst, "size_rnd") == "0.0000", "Size Rnd should be cleared after migration");
    api()->destroy_instance(inst);

    inst = api()->create_instance("", NULL);
    set(inst, "state", "{\"size\": \"0.6000\", \"size_rnd\": \"0.5000\"}");
    CHECK(get(inst, "mode") == "Granular", "old granular state stays Granular");
    CHECK(get(inst, "size_rnd") == "0.5000", "granular Size Rnd kept");
    api()->destroy_instance(inst);

    inst = api()->create_instance("", NULL);
    set(inst, "state", "{\"size\": \"1.0000\", \"mode\": \"Granular\"}");
    CHECK(get(inst, "mode") == "Granular", "an explicit mode wins over Size");
    api()->destroy_instance(inst);
}

static std::string prettify(const std::string &json) {
    std::string out;
    for (char ch : json) {
        out += ch;
        if (ch == ':') out += " ";
        if (ch == ',' || ch == '{') out += "\n    ";
    }
    return out;
}

static void test_state_roundtrip() {
    void *a = api()->create_instance("", NULL);
    set(a, "time", "0.13");
    set(a, "size", "0.77");
    set(a, "pitch", "-7");
    set(a, "density", "0.31");
    set(a, "pitch_rnd", "-0.42");
    set(a, "media", "Clean Tape");
    set(a, "seed", "Clock");
    set(a, "clock_div", "1/16T");
    set(a, "buffer", "Mono");
    set(a, "drive", "6.5");
    set(a, "dry", "Media");
    set(a, "mono_grains", "On");
    set(a, "freeze_save", "Off");
    std::string state = get(a, "state");
    CHECK(state.size() > 10 && state[0] == '{', "state should be JSON: %s", state.c_str());

    void *b = api()->create_instance("", NULL);
    set(b, "state", prettify(state).c_str());
    for (int k = 0; k < P_COUNT; ++k) {
        if (PARAMS[k].type == T_TRIGGER) continue;
        CHECK(get(a, PARAMS[k].key) == get(b, PARAMS[k].key),
              "state round trip: %s %s != %s", PARAMS[k].key,
              get(a, PARAMS[k].key).c_str(), get(b, PARAMS[k].key).c_str());
    }
    CHECK(get(a, "state") == get(b, "state"), "state round trip: freeze_id differs");

    // Garbage must not crash and must keep the current values.
    const char *garbage[] = { "", "{", "nonsense", "{\"pitch\": 5}", "{\"media\":\"\"}" };
    for (const char *g : garbage) set(b, "state", g);
    CHECK(get(b, "size") == get(a, "size"), "garbage state changed size");
    api()->destroy_instance(a);
    api()->destroy_instance(b);
}

static bool wait_for(bool (*pred)(void *), void *arg, int timeout_ms) {
    for (int t = 0; t < timeout_ms; t += 10) {
        if (pred(arg)) return true;
        struct timespec ts = { 0, 10 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return pred(arg);
}

static bool file_exists(void *path) {
    struct stat st;
    return stat((const char *)path, &st) == 0;
}

static void test_freeze_persistence() {
    setenv("GRESIL_DATA_DIR", "build/test_data", 1);
    void *a = api()->create_instance("", NULL);
    GresilInstance *ia = (GresilInstance *)a;
    unlink(ia->store.path);
    set(a, "media", "Vintage");
    Source src;
    run(a, &src, 2.0f);
    set(a, "freeze", "On");
    run(a, &src, kFreezeSaveDelay + 0.2f);
    bool saved = wait_for(file_exists, ia->store.path, 3000);
    CHECK(saved, "frozen buffer should be saved to %s", ia->store.path);
    std::string state = get(a, "state");

    void *b = api()->create_instance("", NULL);
    GresilInstance *ib = (GresilInstance *)b;
    set(b, "state", prettify(state).c_str());
    int16_t buf[256];
    for (int i = 0; i < 300 && (ib->pending_load || ib->loading); ++i) {
        src.fill(buf, 128);
        api()->process_block(b, buf, 128);
        struct timespec ts = { 0, 5 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    CHECK(!ib->loading, "load should complete");
    CHECK(get(b, "freeze") == "On", "restored instance should stay frozen");
    // The last 2KB of bank 3 is Beads' freeze tail (input captured while
    // frozen, used to crossfade on unfreeze): it is scratch, not content.
    const size_t freeze_tail = 256 * sizeof(FloatFrame);
    CHECK(memcmp(ia->delay_memory, ib->delay_memory, kDelayMemorySize - freeze_tail) == 0,
          "restored buffer should match the frozen one");
    CHECK(*ia->processor.mutable_delay_write_ptr() == *ib->processor.mutable_delay_write_ptr(),
          "restored write pointer should match");

    // Unfreezing before the save delay must not write a file.
    void *c = api()->create_instance("", NULL);
    GresilInstance *ic = (GresilInstance *)c;
    unlink(ic->store.path);
    set(c, "freeze", "On");
    run(c, &src, 0.5f);
    set(c, "freeze", "Off");
    run(c, &src, kFreezeSaveDelay + 0.5f);
    struct timespec ts = { 0, 200 * 1000 * 1000 };
    nanosleep(&ts, NULL);
    CHECK(!file_exists(ic->store.path), "short freeze should not be saved");

    // A missing backup wakes up unfrozen.
    void *d = api()->create_instance("", NULL);
    GresilInstance *id = (GresilInstance *)d;
    set(d, "state", "{\"freeze\": \"On\", \"freeze_id\": \"deadbeef00000000\"}");
    for (int i = 0; i < 300 && (id->pending_load || id->loading); ++i) {
        src.fill(buf, 128);
        api()->process_block(d, buf, 128);
        nanosleep(&ts, NULL);
    }
    CHECK(get(d, "freeze") == "Off", "missing backup should unfreeze");

    api()->destroy_instance(a);
    api()->destroy_instance(b);
    api()->destroy_instance(c);
    api()->destroy_instance(d);
}

static void test_knob_overlay() {
    void *inst = api()->create_instance("", NULL);
    set(inst, "_level", "Beads");
    CHECK(get(inst, "knob_2_value") == "75%", "size display: %s", get(inst, "knob_2_value").c_str());
    set(inst, "knob_2_adjust", "30");
    CHECK(get(inst, "mode") == "Granular", "Size at max must no longer switch to delay");
    set(inst, "_level", "Setup");
    CHECK(get(inst, "knob_1_name") == "Mode", "setup knob 1 is Mode");
    set(inst, "knob_1_adjust", "1");
    CHECK(get(inst, "mode") == "Delay", "mode knob should select Delay");
    set(inst, "_level", "Beads");
    CHECK(get(inst, "knob_2_name") == "Filter", "delay mode: Beads knob 2 should read Filter");
    set(inst, "_level", "Random");
    CHECK(get(inst, "knob_2_name") == "Filter", "delay mode: Random knob 2 should read Filter");
    set(inst, "knob_2_adjust", "-30");
    CHECK(get(inst, "knob_2_value") == "LP 30%", "filter display: %s", get(inst, "knob_2_value").c_str());
    CHECK(get(inst, "size_rnd") == "0.0000", "the filter must not move Size Rnd");
    set(inst, "_level", "Setup");
    set(inst, "knob_8_adjust", "1");  // Trim
    Source src;
    run(inst, &src, 0.1f);
    CHECK(get(inst, "trim") == "Off", "trim is momentary");
    CHECK(get(inst, "nonexistent") == "<none>", "unknown keys return -1");
    CHECK(get(inst, "chain_params") == "<none>", "chain_params falls back to module.json");
    api()->destroy_instance(inst);
}

static void test_media_switch_while_running() {
    void *inst = api()->create_instance("", NULL);
    Source src;
    Stats st;
    const char *media[] = { "Dirty Tape", "Digital", "Clean Tape", "Vintage" };
    for (int i = 0; i < 8; ++i) {
        set(inst, "media", media[i % 4]);
        set(inst, "buffer", (i & 1) ? "Mono" : "Stereo");
        set(inst, "source", (i % 3 == 0) ? "Synth" : "Input");
        run(inst, &src, 0.3f, &st);
    }
    CHECK(st.peak <= 32767, "no overflow");
    api()->destroy_instance(inst);
}

int main() {
    printf("Gresil native tests\n");
    test_media_and_modes();
    test_default_grain_density();
    test_random_knobs();
    test_click_free_controls();
    test_audible_after_media_change();
    test_mode_and_filter();
    test_mode_migration();
    test_clean_dry_is_transparent();
    test_synth_source();
    test_clock_pll();
    test_clock_seed_runs();
    test_state_roundtrip();
    test_freeze_persistence();
    test_knob_overlay();
    test_media_switch_while_running();
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
