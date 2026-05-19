/**
 * @file tests/unit/test_stat_trackers.cpp
 * @brief Test src/stat_trackers.*.
 */
#include "../tests_common.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <src/stat_trackers.h>

namespace {

  std::vector<std::string> split_csv(const std::string &line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
      fields.push_back(field);
    }
    return fields;
  }

  struct CsvStatsLoggerTest: testing::Test {
    std::filesystem::path test_dir;

    void SetUp() override {
      const auto *info = testing::UnitTest::GetInstance()->current_test_info();
      test_dir = std::filesystem::temp_directory_path() / "sunshine_stats_tests" / info->name();
      std::filesystem::remove_all(test_dir);
    }

    void TearDown() override {
      std::filesystem::remove_all(test_dir);
    }

    /**
     * @brief Read the non-empty lines of the single CSV file produced under test_dir.
     */
    std::vector<std::string> read_csv_lines() {
      std::vector<std::string> lines;
      if (!std::filesystem::exists(test_dir)) {
        return lines;
      }
      for (const auto &entry : std::filesystem::directory_iterator(test_dir)) {
        std::ifstream file(entry.path());
        std::string line;
        while (std::getline(file, line)) {
          if (!line.empty()) {
            lines.push_back(line);
          }
        }
        break;  // a session writes exactly one file
      }
      return lines;
    }
  };

  TEST_F(CsvStatsLoggerTest, DisabledWhenPathEmpty) {
    stat_trackers::csv_stats_logger logger;
    logger.start("");
    logger.record_frame(1000, 5.0, true);
    logger.record_fec_status(3, false);
    logger.record_idr_request();
    logger.stop();

    // An empty path disables the export entirely; nothing is created.
    EXPECT_FALSE(std::filesystem::exists(test_dir));
  }

  TEST_F(CsvStatsLoggerTest, WritesHeaderOnStart) {
    stat_trackers::csv_stats_logger logger;
    logger.start(test_dir.string());
    logger.stop();

    auto lines = read_csv_lines();
    ASSERT_EQ(lines.size(), 1u);  // header only, no frames recorded
    EXPECT_EQ(
      lines[0],
      "elapsed_ms,frames,encoded_kb,bitrate_kbps,avg_encode_ms,"
      "idr_frames,fec_events,missing_packets,unrecoverable_frames,idr_requests"
    );
  }

  TEST_F(CsvStatsLoggerTest, AccumulatesIntoFinalRow) {
    stat_trackers::csv_stats_logger logger;
    logger.start(test_dir.string());

    logger.record_frame(1024, 4.0, true);
    logger.record_frame(2048, 6.0, false);
    logger.record_frame(1024, 5.0, false);
    logger.record_fec_status(3, false);
    logger.record_fec_status(7, true);
    logger.record_idr_request();

    // stop() flushes the partial interval into a final row.
    logger.stop();

    auto lines = read_csv_lines();
    ASSERT_EQ(lines.size(), 2u);  // header + one data row

    auto fields = split_csv(lines[1]);
    ASSERT_EQ(fields.size(), 10u);
    EXPECT_EQ(fields[1], "3");  // frames
    EXPECT_EQ(fields[5], "1");  // idr_frames
    EXPECT_EQ(fields[6], "2");  // fec_events
    EXPECT_EQ(fields[7], "10");  // missing_packets (3 + 7)
    EXPECT_EQ(fields[8], "1");  // unrecoverable_frames
    EXPECT_EQ(fields[9], "1");  // idr_requests
  }

  TEST_F(CsvStatsLoggerTest, WritesRowAfterTickInterval) {
    stat_trackers::csv_stats_logger logger;
    logger.start(test_dir.string());

    logger.record_frame(1000, 1.0, false);

    // Cross the tick boundary so the next frame triggers a row write.
    std::this_thread::sleep_for(stat_trackers::csv_stats_logger::tick_interval + std::chrono::milliseconds(50));

    logger.record_frame(1000, 1.0, false);  // writes the first interval's row (frames = 2)
    logger.record_frame(1000, 1.0, false);  // accumulates into a new interval (frames = 1)

    logger.stop();  // flushes the partial interval (frames = 1)

    auto lines = read_csv_lines();
    ASSERT_EQ(lines.size(), 3u);  // header + tick row + final partial row

    EXPECT_EQ(split_csv(lines[1])[1], "2");
    EXPECT_EQ(split_csv(lines[2])[1], "1");
  }

  TEST_F(CsvStatsLoggerTest, RecordsAreNoOpsAfterStop) {
    stat_trackers::csv_stats_logger logger;
    logger.start(test_dir.string());
    logger.stop();

    // A late frame from a lingering broadcast packet must not be recorded.
    logger.record_frame(1000, 1.0, true);
    logger.record_fec_status(5, true);

    EXPECT_EQ(read_csv_lines().size(), 1u);  // still header only
  }

}  // namespace
