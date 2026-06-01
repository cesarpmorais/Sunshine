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
 * See [[idea-10-abr-comparison]] for the policy roadmap.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstdint>
#include <optional>

namespace bitrate {

  /**
   * @brief Per-session adaptive bitrate controller.
   *
   * Lifetime is the streaming session: constructed in `session::alloc()` with
   * the negotiated bitrate as the initial target. The current policy is the
   * trivial "hold the negotiated bitrate" rule — `on_sample` always returns
   * `std::nullopt`. The class exists so future policies (rule-based AIMD,
   * RL, …) drop in without disturbing the surrounding plumbing.
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
     */
    explicit bitrate_controller_t(int initial_kbps);

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

  private:
    int initial_kbps;
    int current_kbps;
  };

}  // namespace bitrate
