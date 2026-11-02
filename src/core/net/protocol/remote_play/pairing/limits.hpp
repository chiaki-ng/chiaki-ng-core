/// @file
/// @brief Fixed values of Remote Play registration (pairing): timeouts,
/// retries and the sizes of the registration request and reply.
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::net::protocol::remote_play {

/// @brief How long one phase of a home pairing exchange or knock may take, on
/// the monotonic clock (3 s).
inline constexpr std::chrono::microseconds phase_timeout{3'000'000};
/// @brief How long home pairing waits after the console answers the knock
/// before connecting (100 ms).
inline constexpr std::chrono::microseconds search_delay{100'000};
/// @brief The longest home pairing sleeps between polls while knocking,
/// connecting, sending or receiving.
inline constexpr std::chrono::milliseconds timer_tick{50};
/// @brief The size of the buffer the knock's answer is read into, in bytes.
inline constexpr std::size_t search_response_buffer_size = 256;
/// @brief How many times PSN (RUDP) registration sends its closing
/// acknowledgement before giving up waiting for the console's finish.
inline constexpr int finish_attempt_count = 3;
/// @brief How long PSN registration waits for the console after each closing
/// acknowledgement (1 s).
inline constexpr std::chrono::microseconds finish_retry_wait{1'000'000};
/// @brief Replies of this many bytes or fewer to the closing acknowledgement
/// end the wait, as does a finish packet.
inline constexpr std::size_t min_finish_response_size = 8;
/// @brief The longest host accepted in the registration request's `HOST`
/// header, in characters (an IPv6 address without zone).
inline constexpr std::size_t max_host_length = 45;
/// @brief The size of the registration payload's cleartext prefix, in bytes:
/// filler `A`s with the IV-seed-derived "aeropause" bytes planted in it.
inline constexpr std::size_t request_prefix_size = 480;
/// @brief The capacity of the encrypted part of the registration payload,
/// one byte of which is reserved; in bytes.
inline constexpr std::size_t request_inner_size = 544;
/// @brief The length of an 8-byte account id in standard base64.
inline constexpr std::size_t base64_account_size = 12;
/// @brief The largest registration reply accepted, head and body together, in
/// bytes.
inline constexpr std::size_t max_response_size = 1500;

}  // namespace libchiaking::net::protocol::remote_play
