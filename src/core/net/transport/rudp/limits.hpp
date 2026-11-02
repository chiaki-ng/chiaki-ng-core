/// @file
/// @brief Sizes and retry budgets of RUDP, the reliable-UDP framing a PS5 uses
/// for the Remote Play session and ctrl exchange over the internet.
#pragma once

#include "libchiaking/detail/wire_sizes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace libchiaking::net {

/// @brief Bytes of the random nonce in a switch-to-stream request: 16.
inline constexpr std::size_t rudp_nonce_size = rp_nonce_size;
/// @brief Most times the closing acknowledgement of a session-init exchange is
/// sent while waiting for the console's finish packet.
inline constexpr int rudp_finish_attempts = 3;
/// @brief Most times each handshake request (init, then cookie) is sent before
/// the handshake gives up.
inline constexpr int rudp_max_sends = 3;
/// @brief How long the ctrl channel waits for the console to confirm a switch
/// from RUDP to the Takion stream: 5 seconds.
inline constexpr std::chrono::microseconds rudp_switch_timeout{5'000'000};

}  // namespace libchiaking::net
