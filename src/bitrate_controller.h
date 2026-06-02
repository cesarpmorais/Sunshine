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
 * The policy is the rule-based AIMD described in [[idea-10-abr-comparison]]:
 *   - decrease: continuous RTT > threshold for `hysteresis_ms` → multiply by β
 *   - increase: continuous RTT < (threshold − margin) for `stable_time_ms`
 *               → phase-1 multiplicative recovery (×γ) until 90 % of the
 *               last known good, then phase-2 linear probe (+step)
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
   *   - `recovering` Most recent decision was a phase-1 multiplicative recovery.
   *   - `probing`    Most recent decision was a phase-2 linear probe.
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
    int stable_time_ms = 7500;  ///< continuous clear time before increase fires
    double beta = 0.7;  ///< multiplicative decrease factor (< 1)
    double gamma = 1.15;  ///< phase-1 multiplicative recovery factor (> 1)
    int step_kbps = 500;  ///< phase-2 linear probe step
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

    state_t state = state_t::stable;
  };

}  // namespace bitrate
