#include "src/core/net/nat/holepunch/holepunch_conn_request.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_accept.hpp"
#include "src/core/utils/json/wire_dialect.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <string>
#include <string_view>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using internet::holepunch::Accept;
using nat::Candidate;
using nat::CandidateType;
using SelectedError = internet::holepunch::SelectedAcceptError;

class HolepunchAcceptCandidate : public ::testing::Test {
 protected:
  const Candidate local_{CandidateType::local, "192.168.1.2",
                         "wrong-local-mapped", 1000, 2000};
  const Candidate remote_{CandidateType::stun, "198.51.100.3",
                          "wrong-remote-mapped", 3000, 4000};
};

TEST_F(HolepunchAcceptCandidate, MapsALocalCandidateToOurLocalAddress) {
  const Candidate selected{CandidateType::local, "172.16.0.9", "old-mapped",
                           5000, 6000};
  const auto result = Accept::prepare_candidate(selected, local_, remote_);
  EXPECT_EQ(result.type, CandidateType::local);
  EXPECT_EQ(result.addr, "172.16.0.9");
  EXPECT_EQ(result.port, 5000);
  EXPECT_EQ(result.mapped_addr, "192.168.1.2");
  EXPECT_EQ(result.mapped_port, 1000);
}

TEST_F(HolepunchAcceptCandidate, MapsAPrivateDerivedAddressToTheRemote) {
  for (const std::string_view address :
       {"10.0.0.5", "192.168.1.42", "172.16.4.1", "172.31.2.3", "FD00::1",
        "fc12::"}) {
    SCOPED_TRACE(address);
    const Candidate selected{CandidateType::derived, std::string{address},
                             "old-mapped", 7, 8};
    const auto result = Accept::prepare_candidate(selected, local_, remote_);
    EXPECT_EQ(result.addr, address);
    EXPECT_EQ(result.port, 7);
    EXPECT_EQ(result.mapped_addr, "198.51.100.3");
    EXPECT_EQ(result.mapped_port, 3000);
  }
}

TEST_F(HolepunchAcceptCandidate, MapsABarePrefixDerivedAddressToTheLocal) {
  for (const std::string_view address :
       {"10.", "192.168.", "172.16.", "172.31.", "FC", "fc", "FD", "fd"}) {
    SCOPED_TRACE(address);
    const Candidate selected{CandidateType::derived, std::string{address}, "",
                             7, 8};
    const auto result = Accept::prepare_candidate(selected, local_, remote_);
    EXPECT_EQ(result.mapped_addr, "192.168.1.2");
    EXPECT_EQ(result.mapped_port, 1000);
  }
}

TEST_F(HolepunchAcceptCandidate, MapsStaticAndStunCandidatesToTheRemote) {
  for (const auto type : {CandidateType::static_, CandidateType::stun}) {
    SCOPED_TRACE(static_cast<int>(type));
    const Candidate selected{type, "10.", "old-mapped", 11, 12};
    const auto result = Accept::prepare_candidate(selected, local_, remote_);
    EXPECT_EQ(result.type, type);
    EXPECT_EQ(result.addr, "10.");
    EXPECT_EQ(result.port, 11);
    EXPECT_EQ(result.mapped_addr, "198.51.100.3");
    EXPECT_EQ(result.mapped_port, 3000);
  }
}

class HolepunchAcceptRequest : public ::testing::Test {
 protected:
  static constexpr std::string_view spelling =
      R"({"sid":16909060,"peerSid":168496141,"skey":"AAAAAAAAAAAAAAAAAAAAAA==","natType":0,"candidate":[{"type":"LOCAL","addr":"192.168.1.42","mappedAddr":"203.0.113.7","port":12345,"mappedPort":54321}],"defaultRouteMacAddr":"","localPeerAddr":{"accountId":"1234567890123456789","platform":"REMOTE_PLAY"},"localHashedId":""})";

  const nat::ConnRequest request_ = Accept::build_request(
      16909060U, 168496141U,
      {CandidateType::local, "192.168.1.42", "old-mapped", 12345, 1},
      {CandidateType::local, "203.0.113.7", "wrong", 54321, 9999},
      {CandidateType::stun, "198.51.100.9", "wrong", 40000, 8888});
};

TEST_F(HolepunchAcceptRequest, CarriesOnlyTheSelectedCandidate) {
  EXPECT_EQ(request_.sid, 16909060U);
  EXPECT_EQ(request_.peer_sid, 168496141U);
  EXPECT_EQ(request_.nat_type, 0U);
  ASSERT_EQ(request_.candidates.size(), 1U);
  for (const auto byte : request_.default_route_mac)
    EXPECT_EQ(byte, std::byte{});
  for (const auto byte : request_.skey) EXPECT_EQ(byte, std::byte{});
  for (const auto byte : request_.local_hashed_id) EXPECT_EQ(byte, std::byte{});
  EXPECT_EQ(request_.candidates[0].mapped_addr, "203.0.113.7");
  EXPECT_EQ(request_.candidates[0].mapped_port, 54321U);
}

TEST_F(HolepunchAcceptRequest, SpellsTheConnRequestTheConsoleExpects) {
  const auto wire =
      nat::ConnRequestCodec::encode(request_, 1234567890123456789LL);
  ASSERT_TRUE(wire.has_value());
  const auto spelled = libchiaking::json::ps::write<
      libchiaking::json::ps::body_options_with_nulls>(wire.value());
  ASSERT_TRUE(spelled);
  EXPECT_EQ(spelled.value().view(), spelling);
  const auto parsed =
      libchiaking::json::WireDialect::read<nat::ConnRequestWire>(
          spelled.value().view());
  ASSERT_TRUE(parsed.has_value());
  const auto decoded = nat::ConnRequestCodec::decode(parsed.value());
  ASSERT_TRUE(decoded.has_value());
  EXPECT_EQ(decoded.value().sid, request_.sid);
  EXPECT_EQ(decoded.value().peer_sid, request_.peer_sid);
  ASSERT_EQ(decoded.value().candidates.size(), 1U);
  EXPECT_EQ(decoded.value().candidates[0].mapped_addr, "203.0.113.7");
  EXPECT_EQ(decoded.value().candidates[0].mapped_port, 54321U);
}

class HolepunchSelectedAccept : public ::testing::Test {
 protected:
  HolepunchSelectedAccept() {
    ours_.local = {CandidateType::local, "192.168.1.42", "0.0.0.0", 54321, 0};
    ours_.request.sid = 4660;
    ours_.request.candidates = {
        {CandidateType::stun, "203.0.113.7", "0.0.0.0", 40000, 0},
        {CandidateType::static_, "203.0.113.7", "0.0.0.0", 54321, 0},
        ours_.local};
    console_.sid = 22136;
    console_.candidates = {
        {CandidateType::stun, "198.51.100.9", "0.0.0.0", 40000, 0},
        {CandidateType::local, "10.0.0.5", "0.0.0.0", 33333, 0}};
  }

  auto build(std::size_t index) {
    selected_.candidate_index = index;
    return Accept::build_selected(ours_, console_, selected_);
  }

  internet::holepunch::PreparedOffer ours_{};
  nat::ConnRequest console_{};
  nat::SelectedCandidate selected_{};
};

TEST_F(HolepunchSelectedAccept, OffersTheConsolesLocalCandidateWeReached) {
  auto local = build(1);
  ASSERT_TRUE(local);
  EXPECT_EQ(local.value().sid, 4660);
  EXPECT_EQ(local.value().peer_sid, 22136);
  ASSERT_EQ(local.value().candidates.size(), 1U);
  EXPECT_EQ(local.value().candidates[0].type, CandidateType::local);
  EXPECT_EQ(local.value().candidates[0].addr, "10.0.0.5");
  EXPECT_EQ(local.value().candidates[0].port, 33333);
  EXPECT_EQ(local.value().candidates[0].mapped_addr, "192.168.1.42");
  EXPECT_EQ(local.value().candidates[0].mapped_port, 54321);
}

TEST_F(HolepunchSelectedAccept, MapsAStunCandidateToOurStunAddress) {
  auto stun = build(0);
  ASSERT_TRUE(stun);
  ASSERT_FALSE(stun.value().candidates.empty());
  EXPECT_EQ(stun.value().candidates[0].type, CandidateType::stun);
  EXPECT_EQ(stun.value().candidates[0].addr, "198.51.100.9");
  EXPECT_EQ(stun.value().candidates[0].port, 40000);
  EXPECT_EQ(stun.value().candidates[0].mapped_addr, "203.0.113.7");
  EXPECT_EQ(stun.value().candidates[0].mapped_port, 40000);
}

TEST_F(HolepunchSelectedAccept, FallsBackToOurStaticPortWithoutAStunOffer) {
  ours_.request.candidates.erase(ours_.request.candidates.begin());
  auto direct = build(0);
  ASSERT_TRUE(direct);
  ASSERT_FALSE(direct.value().candidates.empty());
  EXPECT_EQ(direct.value().candidates[0].mapped_port, 54321);
}

TEST_F(HolepunchSelectedAccept, RefusesACandidateTheConsoleNeverOffered) {
  auto invalid = build(9);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), SelectedError::candidate_index_out_of_range);
}

TEST_F(HolepunchSelectedAccept, RefusesAnOfferWithoutItsLocalCandidate) {
  ours_.request.candidates.pop_back();
  auto invalid = build(0);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), SelectedError::malformed_local_offer);
}

TEST_F(HolepunchSelectedAccept, RefusesASessionIdBeyondSixteenBits) {
  ours_.request.sid = 65536;
  auto invalid = build(0);
  ASSERT_FALSE(invalid);
  EXPECT_EQ(invalid.error(), SelectedError::sid_out_of_range);
}

}  // namespace
}  // namespace play_test
