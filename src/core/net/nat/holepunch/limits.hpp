/// @file
/// @brief Sizes, retry budgets and port ranges of Remote Play hole punching,
/// the
/// UDP rendezvous with a console reached over the internet through PSN.
#pragma once

#include "libchiaking/detail/wire_sizes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace libchiaking::net {

/// @brief Retries of the candidate probe round after the first: 21 rounds in
/// all.
inline constexpr std::uint8_t holepunch_probe_max_retries = 20;
/// @brief How long one probe round waits for an answer, restarted whenever a
/// datagram arrives: 500 ms.
inline constexpr std::chrono::milliseconds holepunch_probe_attempt_timeout{500};
/// @brief How long the chosen socket keeps answering the console's probes
/// after a match, until it falls quiet: 1 second.
inline constexpr std::chrono::seconds holepunch_response_wait{1};
/// @brief The TTL a NAT probe socket is raised to once the console reaches it;
/// it starts at 2 so its probes open the local NAT without reaching the far
/// one.
inline constexpr std::uint8_t holepunch_nat_probe_ttl = 64;
/// @brief Lowest port a guessed or predicted port may take; lower ones wrap
/// from the top.
inline constexpr std::int32_t holepunch_port_floor = 1024;
/// @brief Start of the IANA ephemeral range, where ports guessed past 65535
/// wrap to.
inline constexpr std::int32_t holepunch_ephemeral_start = 49152;
/// @brief Predicted STUN candidates offered when the NAT allocates ports in
/// steps.
inline constexpr std::size_t holepunch_stun_candidate_count = 8;

/// @brief Bytes of a peer's hashed id in a connection request and probe.
inline constexpr std::size_t holepunch_hashed_id_size = 20;
/// @brief Bytes of the random id tying a probe response to its request.
inline constexpr std::size_t holepunch_request_id_size = 5;
/// @brief Characters of a MAC address written `aa:bb:cc:dd:ee:ff`.
inline constexpr std::size_t mac_address_text_length =
    mac_address_size * 3U - 1U;

}  // namespace libchiaking::net
