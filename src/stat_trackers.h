/**
 * @file src/stat_trackers.h
 * @brief Declarations for streaming statistic tracking.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <string>

// lib includes
#include <boost/format.hpp>

namespace stat_trackers {

  boost::format one_digit_after_decimal();

  boost::format two_digits_after_decimal();

  template<typename T>
  class min_max_avg_tracker {
  public:
    using callback_function = std::function<void(T stat_min, T stat_max, double stat_avg)>;

    void collect_and_callback_on_interval(T stat, const callback_function &callback, std::chrono::seconds interval_in_seconds) {
      if (data.calls == 0) {
        data.last_callback_time = std::chrono::steady_clock::now();
      } else if (std::chrono::steady_clock::now() > data.last_callback_time + interval_in_seconds) {
        callback(data.stat_min, data.stat_max, data.stat_total / data.calls);
        data = {};
      }
      data.stat_min = std::min(data.stat_min, stat);
      data.stat_max = std::max(data.stat_max, stat);
      data.stat_total += stat;
      data.calls += 1;
    }

    void reset() {
      data = {};
    }

  private:
    struct {
      std::chrono::steady_clock::time_point last_callback_time = std::chrono::steady_clock::now();
      T stat_min = std::numeric_limits<T>::max();
      T stat_max = std::numeric_limits<T>::min();
      double stat_total = 0;
      uint32_t calls = 0;
    } data;
  };

  /**
   * @brief Thread-safe per-session exporter of streaming statistics to a CSV file.
   *
   * One row is written per fixed time interval (`tick_interval`). Frame stats are
   * fed from the video broadcast thread while client-feedback stats (FEC reports,
   * IDR requests) are fed from the control broadcast thread, so all public methods
   * are mutex-guarded.
   */
  class csv_stats_logger {
  public:
    /// @brief Wall-clock interval covered by each CSV row.
    static constexpr std::chrono::milliseconds tick_interval {200};

    /// @brief Gap between consecutive frames at or above which the stream is treated as stalled.
    static constexpr std::chrono::milliseconds stall_threshold {100};

    csv_stats_logger() = default;
    ~csv_stats_logger();

    csv_stats_logger(const csv_stats_logger &) = delete;
    csv_stats_logger &operator=(const csv_stats_logger &) = delete;

    /**
     * @brief Open a timestamped CSV file under `dir` and begin recording.
     *        An empty `dir` disables the export: the logger stays inactive and
     *        every record_* call becomes a no-op.
     * @param dir Output directory; created if it does not exist.
     * @param target_bitrate_kbps Configured encoder target bitrate, in kbps.
     */
    void start(const std::string &dir, std::uint32_t target_bitrate_kbps);

    /// @brief Write any pending partial row, flush, and close the file.
    void stop();

    /**
     * @brief Record one encoded video frame that was sent to the client.
     *        Writes a CSV row when the current interval has elapsed.
     * @param encoded_bytes Size of the encoded frame in bytes.
     * @param processing_latency_ms Encoder processing latency, in milliseconds.
     * @param is_idr Whether the frame is an IDR/key frame.
     * @param frame_send_ms Wall-clock time spent dispatching the frame, in milliseconds.
     * @param packets_sent Number of network packets (shards) sent for the frame.
     */
    void record_frame(std::size_t encoded_bytes, double processing_latency_ms, bool is_idr, double frame_send_ms, std::uint32_t packets_sent);

    /**
     * @brief Record the latest control-channel round-trip time sample.
     *        Snapshot, not accumulated: each CSV row reports the most recent value.
     * @param rtt_ms Round-trip time, in milliseconds.
     * @param rtt_variance_ms Round-trip time variance, in milliseconds.
     */
    void record_rtt(std::uint32_t rtt_ms, std::uint32_t rtt_variance_ms);

    /**
     * @brief Record one SS_FRAME_FEC_STATUS report received from the client.
     * @param missing_packets Value of missingPacketsBeforeHighestReceived.
     * @param unrecoverable Whether the client could not recover the frame.
     */
    void record_fec_status(std::uint32_t missing_packets, bool unrecoverable);

    /// @brief Record one IDR-frame request received from the client.
    void record_idr_request();

    /// @brief Record one reference-frame-invalidation (RFI) request received from the client.
    void record_rfi_request();

  private:
    /// @brief Write a CSV row and reset the interval accumulators. Caller must hold `mutex`.
    void write_row(std::chrono::steady_clock::time_point now);

    std::mutex mutex;
    std::ofstream file;
    bool active = false;

    std::chrono::steady_clock::time_point session_start;
    std::chrono::steady_clock::time_point interval_start;
    std::optional<std::chrono::steady_clock::time_point> last_frame_time;

    // Session-wide snapshot values (not reset per interval).
    std::uint32_t target_bitrate_kbps = 0;
    std::uint32_t last_rtt_ms = 0;
    std::uint32_t last_rtt_variance_ms = 0;

    struct accumulators {
      std::uint32_t frames = 0;
      std::uint64_t encoded_bytes = 0;
      std::uint64_t packets_sent = 0;
      double latency_sum_ms = 0;
      double frame_send_sum_ms = 0;
      std::uint32_t stalls = 0;
      double stall_ms = 0;
      std::uint32_t idr_frames = 0;
      std::uint32_t fec_events = 0;
      std::uint64_t missing_packets = 0;
      std::uint32_t unrecoverable_frames = 0;
      std::uint32_t idr_requests = 0;
      std::uint32_t rfi_requests = 0;
    } interval;
  };

}  // namespace stat_trackers
