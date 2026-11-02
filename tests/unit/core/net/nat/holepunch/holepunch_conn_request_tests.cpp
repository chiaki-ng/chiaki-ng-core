#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/net/nat/holepunch/holepunch_conn_request_rig.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace nat_test {
namespace {

using namespace libchiaking::net::nat::holepunch;

static_assert(!std::is_copy_constructible_v<ConnRequest>);
static_assert(std::is_move_constructible_v<ConnRequest>);

class HolepunchRequestDecoding : public ConnRequestRig {
 protected:
  static auto decode(std::string_view text) -> std::optional<ConnRequest> {
    auto parsed = read(text);
    if (!parsed) return std::nullopt;
    return ConnRequestCodec::decode(*parsed);
  }

  static auto refuses(std::string_view text) -> ::testing::AssertionResult {
    if (decode(text).has_value())
      return ::testing::AssertionFailure() << "accepted " << text;
    return ::testing::AssertionSuccess();
  }
};

TEST_F(HolepunchRequestDecoding, ReadsAnEmptyRequest) {
  auto result = decode(
      R"({"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().sid, 1U);
  EXPECT_EQ(result.value().peer_sid, 2U);
  EXPECT_EQ(result.value().nat_type, 2);
  EXPECT_EQ(result.value().default_route_mac, (std::array<std::byte, 6>{}));
  EXPECT_TRUE(result.value().candidates.empty());
  EXPECT_TRUE(counts_up(result.value().skey, 16, 0));
  EXPECT_TRUE(counts_up(result.value().local_hashed_id, 20, 0));
}

TEST_F(HolepunchRequestDecoding, ReadsAPopulatedRequest) {
  const auto populated = decode(
      R"({"sid":3,"peerSid":4,"skey":"AAECAwQFBgcICQoLDA0ODw==","natType":0,"candidate":[{"type":"STUN","addr":"a","mappedAddr":"b","port":1234,"mappedPort":5678}],"defaultRouteMacAddr":"01:23:45:67:89:ab","localHashedId":"AAECAwQFBgcICQoLDA0ODxAREhM=","localPeerAddr":null})");
  ASSERT_TRUE(populated.has_value());
  EXPECT_EQ(populated.value().sid, 3U);
  EXPECT_EQ(populated.value().peer_sid, 4U);
  EXPECT_EQ(populated.value().default_route_mac[5], std::byte{0xab});
  ASSERT_EQ(populated.value().candidates.size(), 1U);
  EXPECT_EQ(populated.value().candidates[0].port, 1234);
  EXPECT_TRUE(counts_up(populated.value().skey, 16, 16));
  EXPECT_TRUE(counts_up(populated.value().local_hashed_id, 20, 20));
}

TEST_F(HolepunchRequestDecoding, RefusesARequestMissingAField) {
  EXPECT_TRUE(refuses(
      R"({"sid":1,"peerSid":2,"natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
  EXPECT_TRUE(refuses(
      R"({"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
  EXPECT_TRUE(refuses(
      R"({"sid":1,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
  EXPECT_TRUE(refuses(
      R"({"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","candidate":[],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
  EXPECT_TRUE(refuses(
      R"({"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
  EXPECT_TRUE(refuses(
      R"({"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[],"defaultRouteMacAddr":""})"));
  EXPECT_TRUE(refuses(R"([])"));
  EXPECT_TRUE(refuses(R"({})"));
}

TEST_F(HolepunchRequestDecoding, RefusesAMalformedCandidate) {
  EXPECT_TRUE(refuses(
      R"({"sid":1,"peerSid":2,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[{"garbage":true}],"defaultRouteMacAddr":"","localHashedId":"AAAAAAAAAAAAAAAAAAAAAAAAAAA="})"));
}

class HolepunchRequestEncoding : public ConnRequestRig {
 protected:
  void SetUp() override {
    request.sid = 16909060U;
    request.peer_sid = 168496141U;
    request.nat_type = 2;
    for (std::size_t index = 0; index < request.skey.size(); ++index)
      request.skey[index] = static_cast<std::byte>(index);
    request.candidates.push_back(
        {CandidateType::local, "192.168.1.42", "203.0.113.7", 12345, 54321});
  }

  static auto spelled(const ConnRequestEntry& entry) -> std::string {
    const auto written = libchiaking::json::ps::write<
        libchiaking::json::ps::body_options_with_nulls>(entry);
    EXPECT_TRUE(written);
    if (!written) return {};
    return std::string{written.value().view()};
  }

  static auto reread(const ConnRequestEntry& entry) -> Wire {
    auto reread = read(spelled(entry));
    EXPECT_TRUE(reread.has_value());
    if (!reread) return {};
    return std::move(*reread);
  }

  ConnRequest request{};
};

TEST_F(HolepunchRequestEncoding, LeavesAZeroHashedIdEmpty) {
  auto zero = ConnRequestCodec::encode(request, 1234567890123456789LL);
  ASSERT_TRUE(zero.has_value());
  EXPECT_EQ(
      spelled(zero.value()),
      R"({"sid":16909060,"peerSid":168496141,"skey":"AAECAwQFBgcICQoLDA0ODw==","natType":2,"candidate":[{"type":"LOCAL","addr":"192.168.1.42","mappedAddr":"203.0.113.7","port":12345,"mappedPort":54321}],"defaultRouteMacAddr":"","localPeerAddr":{"accountId":"1234567890123456789","platform":"REMOTE_PLAY"},"localHashedId":""})");
  auto decoded_zero = ConnRequestCodec::decode(reread(zero.value()));
  ASSERT_TRUE(decoded_zero.has_value());
  EXPECT_TRUE(counts_up(decoded_zero.value().local_hashed_id, 20, 0));
}

TEST_F(HolepunchRequestEncoding, WritesAHashedIdAndAccountThatDecodeBack) {
  for (std::size_t index = 0; index < request.local_hashed_id.size(); ++index)
    request.local_hashed_id[index] = static_cast<std::byte>(index);
  auto nonzero = ConnRequestCodec::encode(request, -42);
  ASSERT_TRUE(nonzero.has_value());
  const auto nonzero_text = spelled(nonzero.value());
  EXPECT_NE(
      nonzero_text.find(R"("localHashedId":"AAECAwQFBgcICQoLDA0ODxAREhM=")"),
      std::string_view::npos);
  EXPECT_NE(nonzero_text.find(R"("accountId":"-42","platform":"REMOTE_PLAY")"),
            std::string_view::npos);
  auto decoded = ConnRequestCodec::decode(reread(nonzero.value()));
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().sid, request.sid);
  EXPECT_EQ(decoded.value().peer_sid, request.peer_sid);
  EXPECT_EQ(decoded.value().nat_type, request.nat_type);
  ASSERT_EQ(decoded.value().candidates.size(), 1U);
  EXPECT_EQ(decoded.value().candidates[0].port, 12345U);
  for (std::size_t index = 0; index < request.skey.size(); ++index)
    EXPECT_EQ(decoded.value().skey[index], request.skey[index]) << index;
  for (std::size_t index = 0; index < request.local_hashed_id.size(); ++index)
    EXPECT_EQ(decoded.value().local_hashed_id[index],
              request.local_hashed_id[index])
        << index;
}

TEST_F(HolepunchRequestEncoding, RefusesAnOverlongCandidateAddress) {
  request.candidates[0].addr = std::string(46U, 'a');
  auto invalid = ConnRequestCodec::encode(request, 42);
  ASSERT_FALSE(invalid.has_value());
  EXPECT_EQ(invalid.error(), ConnRequestEncodeError::invalid_candidate);
}

TEST_F(HolepunchRequestEncoding, RefusesARequestThatIsTooLarge) {
  request.candidates[0].addr = std::string(45U, '1');
  request.candidates[0].mapped_addr = std::string(45U, '2');
  const auto large_candidate = request.candidates[0];
  request.candidates.assign(120U, large_candidate);
  auto too_large = ConnRequestCodec::encode(request, 42);
  ASSERT_FALSE(too_large.has_value());
  EXPECT_EQ(too_large.error(), ConnRequestEncodeError::too_large);
}

}  // namespace
}  // namespace nat_test
