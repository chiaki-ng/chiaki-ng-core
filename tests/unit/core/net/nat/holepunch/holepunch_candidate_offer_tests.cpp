#include "libchiaking/platform/factory.hpp"
#include "src/core/net/nat/holepunch/holepunch_candidate.hpp"
#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <utility>
#include <vector>

namespace nat_test {
namespace {

namespace pf = libchiaking::platform;
namespace holepunch = libchiaking::net::nat::holepunch;
namespace stun = libchiaking::net::nat::stun;
using holepunch::CandidateGathering;
using holepunch::CandidateType;

class HolepunchOffer : public ::testing::Test {
 protected:
  auto estimate(pf::IpAddress address, std::uint16_t port) const
      -> stun::StunAllocationEstimate {
    return stun::StunAllocationEstimate{{address, port}, 0, false};
  }

  auto estimate_increment(std::int32_t increment, std::uint16_t port) const
      -> stun::StunAllocationEstimate {
    return stun::StunAllocationEstimate{{public_ip, port}, increment, false};
  }

  auto guesses_then_bases(
      const libchiaking::Expected<std::vector<holepunch::Candidate>,
                                  holepunch::OfferCandidatesError>& result,
      std::initializer_list<std::uint16_t> ports) const
      -> ::testing::AssertionResult {
    if (!result) return ::testing::AssertionFailure() << "no offer";
    if (result.value().size() != ports.size() + 2U)
      return ::testing::AssertionFailure()
             << result.value().size() << " candidates for " << ports.size()
             << " guesses";
    std::size_t index = 0;
    for (const auto port : ports) {
      const auto& item = result.value()[index];
      if (item.type != CandidateType::stun || item.addr != "203.0.113.7" ||
          item.port != port || item.mapped_addr != "0.0.0.0" ||
          item.mapped_port != 0)
        return ::testing::AssertionFailure()
               << "guess " << index << " is " << item.addr << ":" << item.port
               << ", expected port " << port;
      ++index;
    }
    if (result.value()[index].type != CandidateType::static_ ||
        result.value()[index].port != local.port)
      return ::testing::AssertionFailure() << "no static base after guesses";
    if (result.value()[index + 1].type != CandidateType::local ||
        result.value()[index + 1].addr != local.addr ||
        result.value()[index + 1].mapped_addr != "0.0.0.0")
      return ::testing::AssertionFailure() << "no local base last";
    return ::testing::AssertionSuccess();
  }

  auto predicts(const stun::StunAllocationEstimate& allocation,
                std::initializer_list<std::uint16_t> ports) const
      -> ::testing::AssertionResult {
    if (ports.size() != 8U)
      return ::testing::AssertionFailure() << "a prediction has eight guesses";
    return guesses_then_bases(
        CandidateGathering::assemble_offer(local, allocation), ports);
  }

  auto guesses(const stun::StunAllocationEstimate& allocation,
               std::size_t count, std::initializer_list<std::uint16_t> ports,
               bool force = false) const -> ::testing::AssertionResult {
    return guesses_then_bases(
        CandidateGathering::assemble_offer(local, allocation, force, count),
        ports);
  }

  const holepunch::Candidate local{CandidateType::local, "192.168.1.42",
                                   "ignored", 50000, 17};
  const pf::IpAddress public_ip = pf::IpAddress::ipv4({203, 0, 113, 7});
  const stun::StunAllocationEstimate random{{public_ip, 52311}, 0, true};
};

TEST_F(HolepunchOffer, OffersStaticThenLocalWhenThePortIsKept) {
  const auto equal =
      CandidateGathering::assemble_offer(local, estimate(public_ip, 50000));
  ASSERT_TRUE(equal);
  ASSERT_EQ(equal.value().size(), 2U);
  EXPECT_EQ(equal.value()[0].type, CandidateType::static_);
  EXPECT_EQ(equal.value()[0].addr, "203.0.113.7");
  EXPECT_EQ(equal.value()[0].port, 50000);
  EXPECT_EQ(equal.value()[1].type, CandidateType::local);
  EXPECT_EQ(equal.value()[1].addr, local.addr);
  for (const auto& candidate : equal.value()) {
    EXPECT_EQ(candidate.mapped_addr, "0.0.0.0");
    EXPECT_EQ(candidate.mapped_port, 0);
  }
}

TEST_F(HolepunchOffer, LeadsWithTheMappedPortWhenItIsRewritten) {
  const auto rewritten =
      CandidateGathering::assemble_offer(local, estimate(public_ip, 52311));
  ASSERT_TRUE(rewritten);
  ASSERT_EQ(rewritten.value().size(), 3U);
  EXPECT_EQ(rewritten.value()[0].type, CandidateType::stun);
  EXPECT_EQ(rewritten.value()[0].addr, "203.0.113.7");
  EXPECT_EQ(rewritten.value()[0].port, 52311);
  EXPECT_EQ(rewritten.value()[1].type, CandidateType::static_);
  EXPECT_EQ(rewritten.value()[1].port, 50000);
  EXPECT_EQ(rewritten.value()[2].type, CandidateType::local);
}

TEST_F(HolepunchOffer, RefusesAnInvalidLocalCandidate) {
  auto invalid_local = local;
  invalid_local.type = CandidateType::static_;
  EXPECT_FALSE(CandidateGathering::assemble_offer(invalid_local,
                                                  estimate(public_ip, 52311)));
  invalid_local = local;
  invalid_local.port = 0;
  EXPECT_FALSE(CandidateGathering::assemble_offer(invalid_local,
                                                  estimate(public_ip, 52311)));
  invalid_local = local;
  invalid_local.addr.clear();
  EXPECT_FALSE(CandidateGathering::assemble_offer(invalid_local,
                                                  estimate(public_ip, 52311)));
  EXPECT_FALSE(
      CandidateGathering::assemble_offer(invalid_local, random, false, 6));
}

TEST_F(HolepunchOffer, RefusesAnUnusableMapping) {
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, estimate(pf::IpAddress::any_ipv4(), 52311)));
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, estimate(pf::IpAddress::loopback_ipv6(), 52311)));
  EXPECT_FALSE(
      CandidateGathering::assemble_offer(local, estimate(public_ip, 0)));
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, {{pf::IpAddress::any_ipv4(), 52311}, 1, false}));
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, {{pf::IpAddress::any_ipv4(), 52311}, 0, true}, false, 6));
}

TEST_F(HolepunchOffer, PredictsEightPortsAlongTheIncrement) {
  EXPECT_TRUE(
      predicts(estimate_increment(1, 52311),
               {52312, 52313, 52314, 52315, 52316, 52317, 52318, 52319}));
  EXPECT_TRUE(
      predicts(estimate_increment(-1, 52311),
               {52310, 52309, 52308, 52307, 52306, 52305, 52304, 52303}));
}

TEST_F(HolepunchOffer, WrapsPredictionsWithinTheUnprivilegedRange) {
  EXPECT_TRUE(predicts(estimate_increment(2, 65534),
                       {1025, 1027, 1029, 1031, 1033, 1035, 1037, 1039}));
  EXPECT_TRUE(
      predicts(estimate_increment(-2, 1025),
               {65534, 65532, 65530, 65528, 65526, 65524, 65522, 65520}));
  EXPECT_TRUE(predicts(estimate_increment(-2, 1026),
                       {1024, 1022, 1020, 1018, 1016, 1014, 1012, 1010}));
}

TEST_F(HolepunchOffer, RefusesAnIncrementBeyondSixteenBits) {
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, estimate_increment(65536, 52311)));
}

TEST_F(HolepunchOffer, GuessesAroundARandomMappingAlternatingSides) {
  EXPECT_TRUE(guesses(random, 6, {52311, 52312, 52310, 52313, 52309, 52314}));
  EXPECT_TRUE(guesses({{public_ip, 52311}, 0, false}, 6,
                      {52311, 52312, 52310, 52313, 52309, 52314}, true));
}

TEST_F(HolepunchOffer, WrapsRandomGuessesAtTheEdgesOfTheirRange) {
  EXPECT_TRUE(
      guesses({{public_ip, 65535}, 0, true}, 4, {65535, 49152, 65534, 49153}));
  EXPECT_TRUE(
      guesses({{public_ip, 1024}, 0, true}, 4, {1024, 1025, 65534, 1026}));
}

TEST_F(HolepunchOffer, LimitsTheNumberOfRandomGuesses) {
  const auto full =
      CandidateGathering::assemble_offer(local, random, false, 75);
  ASSERT_TRUE(full);
  EXPECT_EQ(full.value().size(), 77U);
  EXPECT_FALSE(CandidateGathering::assemble_offer(local, random, false, 0));
  EXPECT_TRUE(CandidateGathering::assemble_offer(local, random, false, 76));
  EXPECT_FALSE(CandidateGathering::assemble_offer(
      local, random, false, CandidateGathering::max_port_guess_count + 1));
}

TEST_F(HolepunchOffer, FitsTheWidestOfferIntoAFullMessage) {
  const holepunch::Candidate wide_local{CandidateType::local, "255.255.255.253",
                                        "0.0.0.0", 65535, 0};
  const stun::StunAllocationEstimate wide{
      {pf::IpAddress::ipv4({255, 255, 255, 254}), 65535}, 0, true};
  auto widest = CandidateGathering::assemble_offer(
      wide_local, wide, false, CandidateGathering::max_port_guess_count);
  ASSERT_TRUE(widest);
  holepunch::ConnRequest offer{};
  offer.sid = 0xffffffffU;
  offer.peer_sid = 0xffffffffU;
  offer.candidates = std::move(widest.value());
  EXPECT_TRUE(holepunch::MessageCodec::format_full(
      holepunch::Action::offer, 0xffff, 0xffff, offer, INT64_MIN));
}

TEST_F(HolepunchOffer, ShapesEachKindOfOffer) {
  const auto predicted_offer =
      CandidateGathering::assemble_offer(local, estimate_increment(1, 52311));
  ASSERT_TRUE(predicted_offer);
  EXPECT_EQ(predicted_offer.value().size(), 10U);
  EXPECT_EQ(predicted_offer.value()[0].port, 52312);
  const auto random_offer =
      CandidateGathering::assemble_offer(local, random, false, 6);
  ASSERT_TRUE(random_offer);
  EXPECT_EQ(random_offer.value().size(), 8U);
  EXPECT_EQ(random_offer.value()[0].port, 52311);
  const auto static_offer = CandidateGathering::assemble_offer(
      local, estimate(public_ip, local.port));
  ASSERT_TRUE(static_offer);
  EXPECT_EQ(static_offer.value().size(), 2U);
  EXPECT_EQ(static_offer.value()[0].type, CandidateType::static_);
  const auto rewritten_offer =
      CandidateGathering::assemble_offer(local, estimate(public_ip, 52311));
  ASSERT_TRUE(rewritten_offer);
  EXPECT_EQ(rewritten_offer.value().size(), 3U);
  EXPECT_EQ(rewritten_offer.value()[0].type, CandidateType::stun);
  const auto forced_offer = CandidateGathering::assemble_offer(
      local, estimate(public_ip, 52311), true, 6);
  ASSERT_TRUE(forced_offer);
  EXPECT_EQ(forced_offer.value().size(), 8U);
}

}  // namespace
}  // namespace nat_test
