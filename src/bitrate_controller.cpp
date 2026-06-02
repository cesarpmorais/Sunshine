/**
 * @file src/bitrate_controller.cpp
 * @brief Rule-based AIMD bitrate controller — see header for the policy.
 */
// header include
#include "bitrate_controller.h"

// standard includes
#include <algorithm>

namespace bitrate {

  using std::chrono::milliseconds;

  bitrate_controller_t::bitrate_controller_t(int initial_kbps, const params_t &params):
      params {params},
      initial_kbps {initial_kbps},
      current_kbps {initial_kbps},
      last_known_good {initial_kbps},
      effective_max_kbps {params.max_kbps > 0 ? params.max_kbps : initial_kbps} {
  }

  std::optional<int> bitrate_controller_t::on_sample(const sample_t &sample) {
    auto now = sample.now;
    const auto rtt = static_cast<int>(sample.rtt_ms);
    const int margin_threshold = params.rtt_threshold_ms - params.margin_ms;

    // ── Per-sample streak tracking (runs every call, ~60 Hz). ──────────────
    // `rtt_over_since` is set on the first sample of a continuous run with
    // rtt > threshold and cleared the moment a sample falls back to or below
    // the threshold. `rtt_clear_since` mirrors the symmetric condition
    // against (threshold − margin).
    if (rtt > params.rtt_threshold_ms) {
      if (!rtt_over_since) {
        rtt_over_since = now;
      }
    } else {
      rtt_over_since.reset();
    }

    if (rtt < margin_threshold) {
      if (!rtt_clear_since) {
        rtt_clear_since = now;
      }
    } else {
      rtt_clear_since.reset();
    }

    // Between decisions the CSV-visible state tracks the live RTT signal,
    // but a sticky decision state (decreasing/recovering/probing) is left
    // alone so the row that captured the decision keeps its label until the
    // next decision cycle re-evaluates.
    if (state == state_t::stable || state == state_t::congested) {
      state = rtt_over_since ? state_t::congested : state_t::stable;
    }

    // ── 1 Hz decision gate. ────────────────────────────────────────────────
    if (last_decision_at && now - *last_decision_at < milliseconds(params.decision_interval_ms)) {
      return std::nullopt;
    }
    last_decision_at = now;

    // Recompute baseline state from RTT at the start of each decision cycle —
    // this clears any sticky decision label from the previous cycle.
    state = rtt_over_since ? state_t::congested : state_t::stable;

    // ── Decrease branch: sustained congestion. ─────────────────────────────
    if (rtt_over_since && now - *rtt_over_since >= milliseconds(params.hysteresis_ms)) {
      const int new_kbps = std::max(static_cast<int>(current_kbps * params.beta), params.min_kbps);
      if (new_kbps == current_kbps) {
        // Already at the floor — nothing to actuate. Keep state as stable so
        // we do not advertise a decrease that did not happen.
        return std::nullopt;
      }
      last_known_good = current_kbps;
      current_kbps = new_kbps;
      // Re-arm the streak from "now" so a second decrease only fires after
      // another full hysteresis window of continued congestion.
      rtt_over_since = now;
      state = state_t::decreasing;
      return current_kbps;
    }

    // ── Increase branch: sustained clear. ──────────────────────────────────
    if (rtt_clear_since && now - *rtt_clear_since >= milliseconds(params.stable_time_ms)) {
      const int recovery_target = static_cast<int>(0.9 * last_known_good);
      int new_kbps;
      state_t new_state;
      if (current_kbps < recovery_target) {
        // Phase 1: multiplicative recovery toward the last known good.
        new_kbps = std::min(static_cast<int>(current_kbps * params.gamma), effective_max_kbps);
        new_state = state_t::recovering;
      } else {
        // Phase 2: linear probing past the previous ceiling.
        new_kbps = std::min(current_kbps + params.step_kbps, effective_max_kbps);
        new_state = state_t::probing;
      }
      if (new_kbps == current_kbps) {
        // Already at the ceiling.
        return std::nullopt;
      }
      current_kbps = new_kbps;
      rtt_clear_since = now;
      state = new_state;
      return current_kbps;
    }

    return std::nullopt;
  }

}  // namespace bitrate
