/// @file
/// @brief Declares the tag a control message is acknowledged by and the DTLS
/// pairing exporter output, the two link values the Kakusan players name.
#pragma once

#include "libchiaking/detail/secret_array.hpp"

#include <cstddef>
#include <cstdint>

namespace libchiaking::net::kakusan {

/// @brief Bytes exported for pairing.
inline constexpr std::size_t pairing_exporter_size = 32;

/// @brief The caller's tag for a control message, returned when the peer
/// acknowledges it.
using AckToken = std::uint64_t;
/// @brief The DTLS pairing exporter output.
using Exporter = SecretArray<pairing_exporter_size>;

}  // namespace libchiaking::net::kakusan
