#pragma once

#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/utils/text/hex.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string_view>
#include <vector>

namespace takion_test {

class TakionCodecRig {
 public:
  static auto bytes(std::initializer_list<std::uint8_t> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values)
      result.push_back(static_cast<std::byte>(value));
    return result;
  }

  static auto from_hex(std::string_view hex) -> std::vector<std::byte> {
    std::vector<std::byte> result(hex.size() / 2U);
    EXPECT_TRUE(libchiaking::text::decode_hex(hex, result)) << hex;
    return result;
  }

  static auto profile(std::uint32_t version)
      -> libchiaking::net::protocol::takion::WireProfile {
    auto resolved =
        libchiaking::net::protocol::takion::resolve_takion_wire_profile(
            version);
    EXPECT_TRUE(resolved.has_value()) << "version " << version;
    return resolved.value();
  }
};

}  // namespace takion_test
