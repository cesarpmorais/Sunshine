/**
 * @file src/bitrate_controller.h
 * @brief Per-session adaptive bitrate controller.
 *
 * Decides whether and when to change the encoder's target bitrate based on
 * server-observable signals (RTT, frame send latency, etc.). The controller
 * itself is policy-only — it owns no I/O. The caller (currently
 * `videoBroadcastThread` in `stream.cpp`) is responsible for feeding samples
 * via `on_sample`, then raising `bitrate_change_events` and updating
 * `csv_stats.set_target_kbps` when the controller returns a new target.
 *
 * The policy is the rule-based AIMD described in [[idea-10-abr-comparison]],
 * with the CUBIC-shaped increase from [[improvement-roadmap]] Idea 5
 * (paper: [[cubic]] — Ha, Rhee, Xu 2008, ACM SIGOPS OSR):
 *   - decrease: continuous RTT > threshold for `hysteresis_ms` →
 *               fast-convergence update of `last_known_good`, then
 *               `current_kbps ← max(β · current_kbps, min_kbps)`.
 *               Fast convergence (Eq. 3.7) shrinks the ceiling on a
 *               cascade decrease: when the new decrease fires while
 *               `current_kbps < last_known_good` (i.e. we had not yet
 *               recovered), `last_known_good ← current · (1+β)/2`.
 *   - increase: continuous RTT < (threshold − margin) for `stable_time_ms`
 *               → sample the cubic recovery curve at the current elapsed
 *               time since the last decrease:
 *                  B(t) = C · (t − K)³ + last_known_good
 *                  K    = cubic_recovery_ms / 1000           (seconds)
 *                  C    = (1 − β) · last_known_good / K³     (kbps · s⁻³)
 *               where t is wall-clock seconds since `last_decrease_at`.
 *               The curve is concave for t < K (fast catch-up to
 *               last_known_good), plateaus around t = K, and turns
 *               convex for t > K (cautious then accelerating probe
 *               into unexplored capacity). New target is clamped at
 *               [current_kbps, effective_max_kbps] so the curve never
 *               drives the bitrate down.
 *
 * `on_sample` is called per encoded frame (~60 Hz). The controller updates
 * its streak timers every call and runs the decision logic at most once per
 * `decision_interval_ms` (default 1 s) so it is independent of frame rate.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>

namespace bitrate {

  /**
   * @brief Observable state of the controller — exposed for CSV instrumentation.
   *
   * Values:
   *   - `stable`     RTT clean, controller idle.
   *   - `congested`  RTT over threshold, hysteresis timer running, no decision yet.
   *   - `decreasing` Most recent decision was a multiplicative decrease (sticky
   *                  until the next decision cycle re-evaluates).
   *   - `recovering` Most recent decision raised the bitrate while still below
   *                  `last_known_good` (CUBIC concave region).
   *   - `probing`    Most recent decision raised the bitrate past
   *                  `last_known_good` (CUBIC convex region).
   */
  enum class state_t : int {
    stable = 0,
    congested = 1,
    decreasing = 2,
    recovering = 3,
    probing = 4,
  };

  /**
   * @brief Tunable parameters for the AIMD policy.
   *        Default values match the starting points in
   *        [[idea-10-abr-comparison]] §"Rule-based controller algorithm".
   *        All time-valued fields are in milliseconds; all bitrates in kbps.
   */
  struct params_t {
    int rtt_threshold_ms = 100;  ///< RTT above this is treated as congestion
    int hysteresis_ms = 2000;  ///< continuous over-threshold time before decrease fires
    int margin_ms = 20;  ///< RTT must be below (threshold − margin) to count as clear
    int stable_time_ms = 1000;  ///< continuous clear time before the cubic increase is sampled
    double beta = 0.7;  ///< multiplicative decrease factor (< 1)
    int cubic_recovery_ms = 15000;  ///< K — wall-clock time the cubic curve takes to reach `last_known_good` after a decrease
    bool fast_convergence = true;  ///< apply CUBIC Eq. 3.7 (shrink `last_known_good` when a new decrease fires before recovery completes)
    int min_kbps = 1000;  ///< bitrate floor
    int max_kbps = 0;  ///< bitrate ceiling; 0 means "use initial_kbps"
    int decision_interval_ms = 1000;  ///< minimum interval between policy decisions
  };

  /**
   * @brief Per-session adaptive bitrate controller.
   *
   * Constructed in `session::alloc()` with the negotiated bitrate as the
   * initial target and the parameters read from `config::stream`.
   */
  class bitrate_controller_t {
  public:
    /**
     * @brief One per-frame observation fed to the controller.
     *        All fields are server-observable — nothing requires Moonlight
     *        cooperation or clock synchronization.
     */
    struct sample_t {
      std::chrono::steady_clock::time_point now;
      std::uint32_t rtt_ms;
      std::uint32_t rtt_var_ms;
      double frame_send_ms;
      std::size_t encoded_frame_bytes;
      std::uint32_t packets_sent;
      bool frame_is_idr;
    };

    /**
     * @param initial_kbps Negotiated target bitrate at session start, in kbps.
     * @param params       AIMD policy parameters.
     */
    bitrate_controller_t(int initial_kbps, const params_t &params);

    /**
     * @brief Feed one observation. Returns a new target bitrate when the
     *        controller decides to change it; `std::nullopt` to keep the
     *        current target.
     */
    std::optional<int> on_sample(const sample_t &sample);

    /// @brief The bitrate the controller is currently asking the encoder for.
    int current_target_kbps() const {
      return current_kbps;
    }

    /// @brief Current controller state for CSV instrumentation.
    state_t current_state() const {
      return state;
    }

  private:
    params_t params;
    int initial_kbps;
    int current_kbps;
    int last_known_good;  ///< bitrate held just before the most recent decrease
    int effective_max_kbps;  ///< params.max_kbps if > 0, else initial_kbps

    /// @brief Start of the current continuous over-threshold streak; unset when streak is broken.
    std::optional<std::chrono::steady_clock::time_point> rtt_over_since;
    /// @brief Start of the current continuous clear-of-margin streak; unset when streak is broken.
    std::optional<std::chrono::steady_clock::time_point> rtt_clear_since;
    /// @brief Time of the last decision-cycle evaluation; gates the 1 Hz cadence.
    std::optional<std::chrono::steady_clock::time_point> last_decision_at;
    /// @brief Time of the most recent decrease; the origin (`t = 0`) of the cubic
    ///        recovery curve. `nullopt` means no decrease has happened yet, so
    ///        the increase branch has nothing to do (bitrate already at max).
    std::optional<std::chrono::steady_clock::time_point> last_decrease_at;

    state_t state = state_t::stable;
  };

}  // namespace bitrate
