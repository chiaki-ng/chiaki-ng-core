#pragma once

#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace net_kakusan_test {

class ControlMessageTest : public ::testing::Test {
 protected:
  using Error = libchiaking::net::kakusan::Error;
  using Codec = libchiaking::net::kakusan::control::MessageCodec;
  using Message = libchiaking::net::kakusan::control::Message;
  using Json = libchiaking::json::strict::Value;
  using Fixtures = test_support::kakusan::Fixtures;

  static auto flow() -> std::optional<Json> {
    auto document = Json::parse(Fixtures::text("control-flow.json"));
    if (!document) {
      ADD_FAILURE() << "control-flow.json does not parse";
      return std::nullopt;
    }
    return std::move(document).value();
  }

  static auto rejects(std::string_view text, Error expected = Error::protocol)
      -> ::testing::AssertionResult {
    const auto decoded = Codec::decode(text);
    if (decoded) return ::testing::AssertionFailure() << "accepted " << text;
    if (decoded.error() != expected)
      return ::testing::AssertionFailure()
             << "rejected " << text << " with "
             << ::testing::PrintToString(decoded.error()) << " instead of "
             << ::testing::PrintToString(expected);
    return ::testing::AssertionSuccess();
  }

  static auto round_trip(const Message& message) -> std::optional<Message> {
    const auto encoded = Codec::encode_secret(message);
    if (!encoded) {
      ADD_FAILURE() << "does not encode";
      return std::nullopt;
    }
    auto decoded = Codec::decode(encoded.value().view());
    if (!decoded) {
      ADD_FAILURE() << "does not decode " << encoded.value().view();
      return std::nullopt;
    }
    EXPECT_EQ(decoded.value().index(), message.index());
    const auto again = Codec::encode_secret(decoded.value());
    EXPECT_TRUE(again);
    if (again) {
      EXPECT_EQ(again.value().view(), encoded.value().view());
    }
    return std::move(decoded).value();
  }

  test_support::SoftwareCrypto crypto;
};

}  // namespace net_kakusan_test
