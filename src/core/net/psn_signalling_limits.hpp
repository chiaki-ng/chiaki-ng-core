/// @file
/// @brief Bounds on the PSN signalling used to start Remote Play over the
/// internet: how much of the push notification feed is held, and how long a
/// session deletion is awaited.
#pragma once

#include <cstddef>
#include <cstdint>

namespace libchiaking::net {

/// @brief Most push notifications held unread in the push feed at once.
inline constexpr std::size_t psn_push_pending_limit = 64;
/// @brief Most bytes of push notifications held unread in the push feed at
/// once: 256 KiB.
inline constexpr std::size_t psn_push_pending_bytes = 256U * 1024U;
/// @brief How long the signalling service waits for PSN to confirm a session's
/// deletion after releasing it, in microseconds: 3 seconds on the monotonic
/// clock.
inline constexpr std::uint64_t psn_session_deletion_timeout_us = 3'000'000;

}  // namespace libchiaking::net
