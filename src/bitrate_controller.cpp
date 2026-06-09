/**
 * @file src/bitrate_controller.cpp
 * @brief Rule-based AIMD bitrate controller — see header for the policy.
 */
// header include
#include "bitrate_controller.h"

// standard includes
#include <algorithm>
#include <cmath>

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
    // Plain β multiplicative cut. The new target is also subject to fast
    // convergence (CUBIC Eq. 3.7): when a decrease fires while we have not
    // yet recovered to the previous ceiling (`current_kbps < last_known_good`),
    // the new `last_known_good` is set *below* the current rate, not equal to
    // it. This shrinks the recovery target on each cascade so the cubic
    // plateaus closer to the actual carrying capacity instead of forever
    // aiming at a stale ceiling.
    if (rtt_over_since && now - *rtt_over_since >= milliseconds(params.hysteresis_ms)) {
      const int new_kbps = std::max(static_cast<int>(current_kbps * params.beta), params.min_kbps);
      if (new_kbps == current_kbps) {
        // Already at the floor — nothing to actuate. Keep state as stable so
        // we do not advertise a decrease that did not happen.
        return std::nullopt;
      }
      if (params.fast_convergence && current_kbps < last_known_good) {
        last_known_good = static_cast<int>(current_kbps * (1.0 + params.beta) / 2.0);
      } else {
        last_known_good = current_kbps;
      }
      current_kbps = new_kbps;
      last_decrease_at = now;
      // Re-arm the streak from "now" so a second decrease only fires after
      // another full hysteresis window of continued congestion.
      rtt_over_since = now;
      state = state_t::decreasing;
      return current_kbps;
    }

    // ── Increase branch: sustained clear, CUBIC curve. ─────────────────────
    // B(t) = C · (t − K)³ + last_known_good
    //   t  = wall-clock seconds since the last decrease (last_decrease_at).
    //   K  = params.cubic_recovery_ms / 1000
    //   C  = (1 − β) · last_known_good / K³        (so B(0) = β · last_known_good)
    //
    // Concave for t < K (rapid catch-up), plateau at t ≈ K, convex for t > K
    // (cautious then accelerating probe). We clamp the new target at
    // [current_kbps, effective_max_kbps] so the curve never drives the bitrate
    // downward — sampling the curve at any t > 0 should monotonically grow.
    //
    // The 5b (anchor at rtt_clear_since) and 5c (don't reset rtt_clear_since
    // on growth) iterations both made empirical results worse on constrained
    // scenarios; see [[results-idea-5b]] and [[results-idea-5c]]. We keep
    // the original Idea 5 anchor (`last_decrease_at`) and reset
    // `rtt_clear_since` on successful growth to gate the next growth tick
    // by `stable_time_ms`.
    if (last_decrease_at && rtt_clear_since &&
        now - *rtt_clear_since >= milliseconds(params.stable_time_ms)) {
      const double t = std::chrono::duration<double>(now - *last_decrease_at).count();
      const double K = params.cubic_recovery_ms / 1000.0;
      const double C = (1.0 - params.beta) * static_cast<double>(last_known_good) / (K * K * K);
      const double B_t = C * std::pow(t - K, 3) + static_cast<double>(last_known_good);
      const int new_kbps = std::clamp(static_cast<int>(B_t), current_kbps, effective_max_kbps);
      if (new_kbps == current_kbps) {
        // Curve hasn't risen above the current rate this tick (we are still
        // in the concave plateau, or already at the ceiling). No actuation.
        return std::nullopt;
      }
      const state_t new_state = (new_kbps < last_known_good) ? state_t::recovering : state_t::probing;
      current_kbps = new_kbps;
      rtt_clear_since = now;
      state = new_state;
      return current_kbps;
    }

    return std::nullopt;
  }

}  // namespace bitrate
