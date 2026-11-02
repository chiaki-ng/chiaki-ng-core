/// @file
/// @brief The defaults an internet Remote Play start uses for port guessing,
/// NAT
/// probe sockets and its overall deadline.
#pragma once

#include <cstddef>
#include <cstdint>

namespace libchiaking::net::nat::holepunch {

/// @brief Ports offered around the public mapping when the NAT allocates at
/// random.
inline constexpr std::size_t default_port_guess_count = 75;
/// @brief Extra low-TTL sockets opened to punch a randomly allocating NAT.
inline constexpr std::size_t default_nat_probe_socket_count = 250;
/// @brief Default time allowed to start a session over the internet, in
/// microseconds: 30 seconds.
inline constexpr std::uint64_t default_remote_start_timeout_us = 30'000'000;

}  // namespace libchiaking::net::nat::holepunch
