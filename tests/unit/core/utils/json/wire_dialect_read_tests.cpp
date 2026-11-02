#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/utils/json/wire_dialect_rig.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace json_test {

class WireDialectRead : public ::testing::Test {
 protected:
  struct Address {
    std::optional<std::string> ip;
  };

  struct Peer {
    using keys = libchiaking::json::ps::CamelCaseKeys;
    std::string local_hashed_id;
    std::optional<Address> local_peer_addr;
  };

  struct Offer {
    using keys = libchiaking::json::ps::CamelCaseKeys;
    std::string action;
    std::int64_t req_id{};
    Peer conn_request;
  };
};

namespace {

using Dialect = WireDialectRig::Dialect;
using ParseError = WireDialectRig::ParseError;

TEST_F(WireDialectRead, ReadsAnOfferAsTheWireSendsIt) {
  auto offer = Dialect::read<Offer>(
      "\xEF\xBB\xBF{\"action\":\"OFFER\",\"reqId\":002,\"reqId\":7,"
      "\"connRequest\":{\"localPeerAddr\":,\"localHashedId\":\"a\tb\xFF\"}}"
      "trailing");
  ASSERT_TRUE(offer);
  EXPECT_EQ(offer->action, "OFFER");
  EXPECT_EQ(offer->req_id, 7);
  EXPECT_EQ(offer->conn_request.local_hashed_id, "a\tb\xFF");
  ASSERT_TRUE(offer->conn_request.local_peer_addr);
  EXPECT_FALSE(offer->conn_request.local_peer_addr->ip);
}

TEST_F(WireDialectRead, KeepsNulCharactersInText) {
  constexpr char with_nul[] = "{\"localHashedId\":\"a\0b\\u0000\"}";
  auto nul =
      Dialect::read<Peer>(std::string_view{with_nul, sizeof with_nul - 1});
  ASSERT_TRUE(nul);
  EXPECT_EQ(nul->local_hashed_id, std::string_view("a\0b\0", 4));
}

TEST_F(WireDialectRead, RefusesWhatDoesNotFitTheType) {
  EXPECT_FALSE(Dialect::read<Offer>(R"({"reqId":"7"})"));
  EXPECT_FALSE(Dialect::read<Offer>(R"({"localPeerAddr":})"));
  const auto deep = Dialect::read<Offer>(std::string(65, '['));
  ASSERT_FALSE(deep);
  EXPECT_EQ(deep.error(), ParseError::too_deep);
}

}  // namespace
}  // namespace json_test
