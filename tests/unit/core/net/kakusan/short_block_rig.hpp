#pragma once

#include "src/core/net/kakusan/short_block.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace net_kakusan_test {

class ShortBlockTest : public ::testing::Test {
 protected:
  using Json = libchiaking::json::strict::Value;
  using Fixtures = test_support::kakusan::Fixtures;

  void SetUp() override {
    auto parsed = Json::parse(Fixtures::text("blocks.json"));
    ASSERT_TRUE(parsed);
    vectors_.emplace(std::move(parsed).value());
  }

  auto vectors() const -> const Json& { return *vectors_; }

  static auto text(const Json& value, std::string_view key)
      -> const std::string& {
    return Fixtures::string_of(Fixtures::member(value, key));
  }

  static auto number(const Json& value, std::string_view key) -> std::uint64_t {
    const auto* integer = Fixtures::member(value, key).integer();
    if (integer == nullptr) {
      ADD_FAILURE() << key << " is not an integer";
      return 0;
    }
    return *integer;
  }

  static auto number(const Json& value) -> std::uint64_t {
    const auto* integer = value.integer();
    if (integer == nullptr) {
      ADD_FAILURE() << "not an integer";
      return 0;
    }
    return *integer;
  }

  auto block(std::string_view sample) const -> const std::string& {
    return text(Fixtures::member(vectors(), sample), "block");
  }

  static void expect_candidates(
      const libchiaking::net::kakusan::IceParameters& ice, const Json& fields,
      const Json& derived) {
    const auto& expected = *Fixtures::member(fields, "candidates").array();
    const auto& priorities = *Fixtures::member(derived, "priorities").array();
    EXPECT_EQ(ice.port_guesses(), number(fields, "port_guesses"));
    EXPECT_EQ(ice.ufrag().view(), text(derived, "ice_ufrag"));
    EXPECT_EQ(ice.password().view(), text(derived, "ice_password"));
    ASSERT_EQ(ice.candidates().size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
      SCOPED_TRACE(index);
      const auto& candidate = ice.candidates()[index];
      const auto srflx = text(expected[index], "type") == "srflx";
      EXPECT_EQ(candidate.type ==
                    libchiaking::net::kakusan::CandidateType::server_reflexive,
                srflx);
      EXPECT_EQ(libchiaking::platform::to_string(candidate.endpoint.address),
                text(expected[index], "address"));
      EXPECT_EQ(candidate.endpoint.port, number(expected[index], "port"));
      EXPECT_EQ(candidate.priority, number(priorities[index]));
      if (srflx) {
        const auto base = number(expected[index], "base");
        ASSERT_LT(base, ice.candidates().size());
        EXPECT_EQ(candidate.related, ice.candidates()[base].endpoint);
      }
    }
  }

  test_support::SoftwareCrypto crypto;

 private:
  std::optional<Json> vectors_;
};

}  // namespace net_kakusan_test
