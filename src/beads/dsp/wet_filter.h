// Gresil addition (not in upstream Beads).
//
// One-pole high-pass + low-pass on the wet signal only, applied before the
// dry/wet mix so the dry path stays untouched. Same 6 dB/oct design as the
// Verglas Filters page (Mutable Instruments ONE_POLE, coefficient
// 1 - exp(-2*pi*f/sr)). Both stages always run, so engaging or moving a
// filter never restarts from stale state: "HP off" tracks at 5 Hz (inaudible),
// "LP off" passes the input straight through.

#ifndef BEADS_DSP_WET_FILTER_H_
#define BEADS_DSP_WET_FILTER_H_

#include <cmath>

#include "stmlib/stmlib.h"

#include "beads/dsp/frame.h"

namespace beads {

class WetFilter {
 public:
  WetFilter() { }
  ~WetFilter() { }

  void Init() {
    hp_state_.l = hp_state_.r = 0.0f;
    lp_state_.l = lp_state_.r = 0.0f;
    hp_coefficient_ = 0.0f;
    lp_coefficient_ = 1.0f;
  }

  // hp_hz <= 0: off. lp_hz >= 19999: off.
  void set_cutoffs(float hp_hz, float lp_hz, float sample_rate) {
    const float hp = hp_hz < 5.0f ? 5.0f : hp_hz;
    hp_coefficient_ = 1.0f - expf(-6.2831853f * hp / sample_rate);
    lp_coefficient_ = lp_hz >= 19999.0f
        ? 1.0f
        : 1.0f - expf(-6.2831853f * lp_hz / sample_rate);
  }

  inline void Process(FloatFrame* in_out, size_t size) {
    while (size--) {
      hp_state_.l += hp_coefficient_ * (in_out->l - hp_state_.l);
      hp_state_.r += hp_coefficient_ * (in_out->r - hp_state_.r);
      const float l = in_out->l - hp_state_.l;
      const float r = in_out->r - hp_state_.r;
      lp_state_.l += lp_coefficient_ * (l - lp_state_.l);
      lp_state_.r += lp_coefficient_ * (r - lp_state_.r);
      in_out->l = lp_state_.l;
      in_out->r = lp_state_.r;
      ++in_out;
    }
  }

 private:
  FloatFrame hp_state_;
  FloatFrame lp_state_;
  float hp_coefficient_;
  float lp_coefficient_;

  DISALLOW_COPY_AND_ASSIGN(WetFilter);
};

}  // namespace beads

#endif  // BEADS_DSP_WET_FILTER_H_
