#pragma once

#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/utils/json/wire_dialect.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <string_view>

namespace nat_test {

class ConnRequestRig : public ::testing::Test {
 protected:
  using Wire = libchiaking::net::nat::holepunch::ConnRequestWire;

  static auto read(std::string_view json) -> std::optional<Wire> {
    auto parsed = libchiaking::json::WireDialect::read<Wire>(json);
    if (!parsed) return std::nullopt;
    return std::move(parsed.value());
  }

  template <class Bytes>
  static auto counts_up(const Bytes& bytes, std::size_t size,
                        std::size_t decoded) -> ::testing::AssertionResult {
    for (std::size_t index = 0; index < size; ++index) {
      const auto expected = index < decoded ? static_cast<unsigned>(index) : 0U;
      const auto actual = std::to_integer<unsigned>(bytes[index]);
      if (actual != expected)
        return ::testing::AssertionFailure()
               << "byte " << index << " is " << actual << ", expected "
               << expected;
    }
    return ::testing::AssertionSuccess();
  }
};

}  // namespace nat_test
