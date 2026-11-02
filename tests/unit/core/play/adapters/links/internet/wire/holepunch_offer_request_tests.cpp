#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string_view>
#include <utility>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
namespace pf = libchiaking::platform;
using internet::holepunch::Offer;
using nat::CandidateType;

class HolepunchOfferRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view spelling =
      R"({"sid":16909060,"peerSid":168496141,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":2,"candidate":[{"type":"STUN","addr":"198.51.100.9","mappedAddr":"0.0.0.0","port":3478,"mappedPort":0},{"type":"STATIC","addr":"203.0.113.7","mappedAddr":"0.0.0.0","port":12345,"mappedPort":0},{"type":"LOCAL","addr":"192.168.1.42","mappedAddr":"0.0.0.0","port":12345,"mappedPort":0}],"defaultRouteMacAddr":"","localPeerAddr":{"accountId":"1234567890123456789","platform":"REMOTE_PLAY"},"localHashedId":"AAECAwQFBgcICQoLDA0ODxAREhM="})";

  static auto input() -> internet::holepunch::OfferRequestInput {
    internet::holepunch::OfferRequestInput input{};
    input.local_sid = 16909060U;
    input.console_sid = 168496141U;
    for (std::size_t index = 0; index < input.local_hashed_id.size(); ++index)
      input.local_hashed_id[index] = static_cast<std::byte>(index);
    input.candidates = {
        {CandidateType::stun, "198.51.100.9", "0.0.0.0", 3478, 0},
        {CandidateType::static_, "203.0.113.7", "0.0.0.0", 12345, 0},
        {CandidateType::local, "192.168.1.42", "0.0.0.0", 12345, 0},
    };
    return input;
  }
};

TEST_F(HolepunchOfferRequest, CarriesOurSessionAndEveryCandidate) {
  auto built = Offer::build_conn_request(input());
  ASSERT_TRUE(built.has_value());
  const auto& request = built.value();
  EXPECT_EQ(request.sid, 16909060U);
  EXPECT_EQ(request.peer_sid, 168496141U);
  EXPECT_EQ(request.nat_type, 2);
  EXPECT_EQ(request.candidates.size(), 3U);
  for (const auto byte : request.skey) EXPECT_EQ(byte, std::byte{});
  for (const auto byte : request.default_route_mac)
    EXPECT_EQ(byte, std::byte{});
  for (std::size_t index = 0; index < request.local_hashed_id.size(); ++index)
    EXPECT_EQ(request.local_hashed_id[index], static_cast<std::byte>(index));
}

TEST_F(HolepunchOfferRequest, SpellsTheConnRequestTheConsoleExpects) {
  auto built = Offer::build_conn_request(input());
  ASSERT_TRUE(built.has_value());
  const auto& request = built.value();
  const auto json =
      nat::ConnRequestCodec::encode(request, 1234567890123456789LL);
  ASSERT_TRUE(json.has_value());
  const auto spelled = libchiaking::json::ps::write<
      libchiaking::json::ps::body_options_with_nulls>(json.value());
  ASSERT_TRUE(spelled);
  EXPECT_EQ(spelled.value().view(), spelling);
  const auto parsed =
      libchiaking::json::WireDialect::read<nat::ConnRequestWire>(
          spelled.value().view());
  ASSERT_TRUE(parsed.has_value());
  const auto decoded = nat::ConnRequestCodec::decode(parsed.value());
  ASSERT_TRUE(decoded.has_value());
  ASSERT_EQ(decoded.value().candidates.size(), 3U);
  EXPECT_EQ(decoded.value().candidates[0].type, CandidateType::stun);
  EXPECT_EQ(decoded.value().candidates[1].type, CandidateType::static_);
  EXPECT_EQ(decoded.value().candidates[2].type, CandidateType::local);
  for (std::size_t index = 0; index < request.local_hashed_id.size(); ++index)
    EXPECT_EQ(decoded.value().local_hashed_id[index],
              request.local_hashed_id[index]);
}

TEST_F(HolepunchOfferRequest, RefusesFewerThanTwoCandidates) {
  internet::holepunch::OfferRequestInput empty{};
  EXPECT_FALSE(Offer::build_conn_request(std::move(empty)));
  internet::holepunch::OfferRequestInput one{};
  one.candidates.push_back(
      {CandidateType::local, "192.168.1.42", "0.0.0.0", 12345, 0});
  EXPECT_FALSE(Offer::build_conn_request(std::move(one)));
}

class HolepunchOfferPrepare : public ::testing::Test {
 protected:
  static auto gathered() -> nat::GatheredCandidates {
    nat::GatheredCandidates gathered{};
    gathered.local = {CandidateType::local, "192.168.1.42", "0.0.0.0", 12345,
                      0};
    gathered.allocation = {
        {pf::IpAddress::ipv4({198, 51, 100, 9}), 3478}, 2, true};
    gathered.offer_candidates = {
        {CandidateType::stun, "198.51.100.9", "0.0.0.0", 3478, 0},
        {CandidateType::static_, "198.51.100.9", "0.0.0.0", 12345, 0},
        {CandidateType::local, "192.168.1.42", "0.0.0.0", 12345, 0},
    };
    return gathered;
  }

  static auto hashed() -> libchiaking::SecretArray<20> {
    libchiaking::SecretArray<20> hashed{};
    for (std::size_t index = 0; index < hashed.size(); ++index)
      hashed[index] = static_cast<std::byte>(index);
    return hashed;
  }
};

TEST_F(HolepunchOfferPrepare, BuildsTheRequestFromTheGatheredCandidates) {
  auto prepared = Offer::prepare(gathered(), 16909060U, 168496141U, hashed());
  ASSERT_TRUE(prepared);
  const auto& offer = prepared.value();
  EXPECT_EQ(offer.request.sid, 16909060U);
  EXPECT_EQ(offer.request.peer_sid, 168496141U);
  EXPECT_EQ(offer.request.nat_type, 2);
  ASSERT_EQ(offer.request.candidates.size(), 3U);
  EXPECT_EQ(offer.request.candidates[0].type, CandidateType::stun);
  EXPECT_EQ(offer.request.candidates[0].port, 3478);
  EXPECT_EQ(offer.request.candidates[1].type, CandidateType::static_);
  EXPECT_EQ(offer.request.candidates[2].type, CandidateType::local);
  for (const auto byte : offer.request.skey) EXPECT_EQ(byte, std::byte{});
  for (const auto byte : offer.request.default_route_mac)
    EXPECT_EQ(byte, std::byte{});
  for (std::size_t index = 0; index < offer.request.local_hashed_id.size();
       ++index)
    EXPECT_EQ(offer.request.local_hashed_id[index],
              static_cast<std::byte>(index));
}

TEST_F(HolepunchOfferPrepare, KeepsTheLocalCandidateAndAllocation) {
  auto prepared = Offer::prepare(gathered(), 16909060U, 168496141U, hashed());
  ASSERT_TRUE(prepared);
  const auto& offer = prepared.value();
  EXPECT_EQ(offer.local.addr, "192.168.1.42");
  EXPECT_EQ(offer.local.port, 12345);
  EXPECT_EQ(offer.allocation.projected,
            (pf::SocketEndpoint{pf::IpAddress::ipv4({198, 51, 100, 9}), 3478}));
  EXPECT_EQ(offer.allocation.increment, 2);
  EXPECT_TRUE(offer.allocation.random_allocation);
}

TEST_F(HolepunchOfferPrepare, RefusesASingleCandidate) {
  nat::GatheredCandidates one{};
  one.offer_candidates.push_back(
      {CandidateType::local, "192.168.1.42", "0.0.0.0", 12345, 0});
  const auto rejected =
      Offer::prepare(std::move(one), 1U, 2U, libchiaking::SecretArray<20>{});
  ASSERT_FALSE(rejected);
  EXPECT_EQ(rejected.error(), nat::ConnRequestError::invalid_schema);
}

}  // namespace
}  // namespace play_test
