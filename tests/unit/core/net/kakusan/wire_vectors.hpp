#pragma once

#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace net_kakusan_test {

class WireVectors {
 public:
  using Fixtures = test_support::kakusan::Fixtures;
  using Value = libchiaking::json::strict::Value;

  WireVectors()
      : document_(Value::parse(Fixtures::text("wire-vectors.json")).value()),
        connection_(
            test_support::Bytes::unhex_array<16>(text("connection_id_hex"))),
        original_(test_support::Bytes::unhex(text("original_bytes_hex"))) {
    std::ranges::copy(
        test_support::Bytes::unhex_array<32>(text("media_exporter_hex")),
        key_.begin());
  }

  auto text(std::string_view name) const -> const std::string& {
    return Fixtures::string_of(Fixtures::member(document_, name));
  }

  auto fragments() const -> const std::vector<Value>& {
    return *Fixtures::member(document_, "media_fragments_hex").array();
  }

  auto fragment(std::size_t index) const -> std::vector<std::byte> {
    return test_support::Bytes::unhex(Fixtures::string_of(fragments()[index]));
  }

  auto key() const noexcept -> const libchiaking::net::kakusan::MediaKey& {
    return key_;
  }

  auto connection() const noexcept
      -> const libchiaking::net::kakusan::ConnectionId& {
    return connection_;
  }

  auto original() const noexcept -> const std::vector<std::byte>& {
    return original_;
  }

 private:
  Value document_;
  libchiaking::net::kakusan::MediaKey key_;
  libchiaking::net::kakusan::ConnectionId connection_;
  std::vector<std::byte> original_;
};

class WireVectorsTest : public ::testing::Test {
 protected:
  test_support::SoftwareCrypto crypto;
  WireVectors vectors;
};

}  // namespace net_kakusan_test
