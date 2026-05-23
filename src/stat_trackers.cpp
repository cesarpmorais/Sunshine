/**
 * @file src/stat_trackers.cpp
 * @brief Definitions for streaming statistic tracking.
 */
// local includes
#include "stat_trackers.h"

// standard includes
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>

// local includes
#include "logging.h"

namespace stat_trackers {

  boost::format one_digit_after_decimal() {
    return boost::format("%1$.1f");
  }

  boost::format two_digits_after_decimal() {
    return boost::format("%1$.2f");
  }

  namespace {

    /**
     * @brief Build a CSV filename containing the current local date and time.
     */
    std::string timestamped_filename() {
      auto now = std::chrono::system_clock::now();
      auto time = std::chrono::system_clock::to_time_t(now);

      std::tm tm {};
#ifdef _WIN32
      localtime_s(&tm, &time);
#else
      localtime_r(&time, &tm);
#endif

      std::ostringstream ss;
      ss << "sunshine-stats-" << std::put_time(&tm, "%Y%m%d-%H%M%S") << ".csv";
      return ss.str();
    }

  }  // namespace

  csv_stats_logger::~csv_stats_logger() {
    stop();
  }

  void csv_stats_logger::start(const std::string &dir, std::uint32_t target_bitrate_kbps) {
    std::lock_guard lock(mutex);

    if (active) {
      return;
    }
    if (dir.empty()) {
      // Export is disabled when no path is configured.
      return;
    }

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) {
      BOOST_LOG(error) << "Stats export: could not create directory [" << dir << "]: " << ec.message();
      return;
    }

    auto path = std::filesystem::path(dir) / timestamped_filename();
    file.open(path);
    if (!file.is_open()) {
      BOOST_LOG(error) << "Stats export: could not open file [" << path.string() << ']';
      return;
    }

    file << "elapsed_ms,frames,encoded_kb,bitrate_kbps,target_kbps,packets_sent,"
            "avg_encode_ms,avg_send_ms,stalls,stall_ms,rtt_ms,rtt_var_ms,"
            "idr_frames,fec_events,missing_packets,unrecoverable_frames,idr_requests,rfi_requests\n";
    file << std::fixed << std::setprecision(2);

    auto now = std::chrono::steady_clock::now();
    session_start = now;
    interval_start = now;
    interval = {};
    last_frame_time.reset();
    this->target_bitrate_kbps = target_bitrate_kbps;
    last_rtt_ms = 0;
    last_rtt_variance_ms = 0;
    active = true;

    BOOST_LOG(info) << "Stats export: writing streaming stats to [" << path.string() << ']';
  }

  void csv_stats_logger::stop() {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    // Flush whatever partial interval remains so no data is lost on shutdown.
    if (interval.frames > 0 || interval.fec_events > 0 || interval.idr_requests > 0 || interval.rfi_requests > 0) {
      write_row(std::chrono::steady_clock::now());
    }

    file.flush();
    file.close();
    active = false;
  }

  void csv_stats_logger::record_frame(std::size_t encoded_bytes, double processing_latency_ms, bool is_idr, double frame_send_ms, std::uint32_t packets_sent) {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    auto now = std::chrono::steady_clock::now();

    // A long gap since the previous frame means the broadcast stalled.
    if (last_frame_time) {
      auto gap = now - *last_frame_time;
      if (gap >= stall_threshold) {
        interval.stalls += 1;
        interval.stall_ms += std::chrono::duration<double, std::milli>(gap).count();
      }
    }
    last_frame_time = now;

    interval.frames += 1;
    interval.encoded_bytes += encoded_bytes;
    interval.packets_sent += packets_sent;
    interval.latency_sum_ms += processing_latency_ms;
    interval.frame_send_sum_ms += frame_send_ms;
    if (is_idr) {
      interval.idr_frames += 1;
    }

    if (now - interval_start >= tick_interval) {
      write_row(now);
    }
  }

  void csv_stats_logger::record_rtt(std::uint32_t rtt_ms, std::uint32_t rtt_variance_ms) {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    last_rtt_ms = rtt_ms;
    last_rtt_variance_ms = rtt_variance_ms;
  }

  void csv_stats_logger::record_fec_status(std::uint32_t missing_packets, bool unrecoverable) {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    interval.fec_events += 1;
    interval.missing_packets += missing_packets;
    if (unrecoverable) {
      interval.unrecoverable_frames += 1;
    }
  }

  void csv_stats_logger::record_idr_request() {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    interval.idr_requests += 1;
  }

  void csv_stats_logger::set_target_kbps(std::uint32_t kbps) {
    std::lock_guard lock(mutex);
    if (!active) {
      return;
    }
    target_bitrate_kbps = kbps;
  }

  void csv_stats_logger::record_rfi_request() {
    std::lock_guard lock(mutex);

    if (!active) {
      return;
    }

    interval.rfi_requests += 1;
  }

  void csv_stats_logger::write_row(std::chrono::steady_clock::time_point now) {
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - session_start).count();
    auto interval_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - interval_start).count();

    double encoded_kb = interval.encoded_bytes / 1024.0;
    // bits / millisecond is numerically equal to kilobits / second.
    double bitrate_kbps = interval_ms > 0 ? (interval.encoded_bytes * 8.0) / interval_ms : 0.0;
    double avg_encode_ms = interval.frames > 0 ? interval.latency_sum_ms / interval.frames : 0.0;
    double avg_send_ms = interval.frames > 0 ? interval.frame_send_sum_ms / interval.frames : 0.0;

    file << elapsed_ms << ','
         << interval.frames << ','
         << encoded_kb << ','
         << bitrate_kbps << ','
         << target_bitrate_kbps << ','
         << interval.packets_sent << ','
         << avg_encode_ms << ','
         << avg_send_ms << ','
         << interval.stalls << ','
         << interval.stall_ms << ','
         << last_rtt_ms << ','
         << last_rtt_variance_ms << ','
         << interval.idr_frames << ','
         << interval.fec_events << ','
         << interval.missing_packets << ','
         << interval.unrecoverable_frames << ','
         << interval.idr_requests << ','
         << interval.rfi_requests << '\n';
    file.flush();

    interval = {};
    interval_start = now;
  }

}  // namespace stat_trackers
