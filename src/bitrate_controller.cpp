/**
 * @file src/bitrate_controller.cpp
 * @brief Per-session adaptive bitrate controller implementation.
 */
// header include
#include "bitrate_controller.h"

namespace bitrate {

  bitrate_controller_t::bitrate_controller_t(int initial_kbps):
      initial_kbps {initial_kbps},
      current_kbps {initial_kbps} {
  }

  std::optional<int> bitrate_controller_t::on_sample(const sample_t & /*sample*/) {
    // Policy stub: hold the negotiated bitrate for the lifetime of the session.
    // Replace the body with the AIMD rule (or any other policy) when ready —
    // mutate `current_kbps` and `return current_kbps;` on change.
    return std::nullopt;
  }

}  // namespace bitrate
