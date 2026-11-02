#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <variant>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using internet::holepunch::AckPeerOffer;
using internet::holepunch::Offer;
using internet::holepunch::SendOwnOffer;
using EffectError = internet::holepunch::OfferEffectError;
using State = internet::holepunch::OfferWaitState;

TEST(HolepunchOfferEffects, AcknowledgeThePeerThenSendOurOwnOffer) {
  const auto result = Offer::make_initial_effects(7, 0x1111, 0x2222);
  ASSERT_TRUE(result.has_value());
  const auto& effects = result.value();
  const auto* ack = std::get_if<AckPeerOffer>(&effects[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 0x1111);
  EXPECT_EQ(ack->error, 0);
  const auto* own = std::get_if<SendOwnOffer>(&effects[1]);
  ASSERT_TRUE(own);
  EXPECT_EQ(own->attempt, 7U);
  EXPECT_EQ(own->own_req_id, 0x2222);
}

TEST(HolepunchOfferEffects, AcknowledgeAPeerOfferWithoutARequestId) {
  const auto zero_peer = Offer::make_initial_effects(1, 0, 2);
  ASSERT_TRUE(zero_peer);
  const auto* zero_ack = std::get_if<AckPeerOffer>(&zero_peer.value()[0]);
  ASSERT_TRUE(zero_ack);
  EXPECT_EQ(zero_ack->peer_req_id, 0);
}

TEST(HolepunchOfferEffects, RefuseAZeroAttempt) {
  const auto bad_attempt = Offer::make_initial_effects(0, 1, 2);
  ASSERT_FALSE(bad_attempt);
  EXPECT_EQ(bad_attempt.error(), EffectError::invalid_attempt);
}

TEST(HolepunchOfferEffects, RefuseAPeerRequestIdBeyondSixteenBits) {
  const auto bad_peer = Offer::make_initial_effects(1, 0x10000, 2);
  ASSERT_FALSE(bad_peer);
  EXPECT_EQ(bad_peer.error(), EffectError::invalid_peer_req_id);
}

TEST(HolepunchOfferEffects, RefuseAZeroOwnRequestId) {
  const auto bad_own = Offer::make_initial_effects(1, 2, 0);
  ASSERT_FALSE(bad_own);
  EXPECT_EQ(bad_own.error(), EffectError::invalid_own_req_id);
}

class HolepunchOfferWait : public ::testing::Test {
 protected:
  static auto message(nat::Action action, std::uint16_t req_id = 0,
                      std::uint16_t error = 0) -> nat::SessionMessage {
    nat::SessionMessage out{};
    out.action = action;
    out.req_id = req_id;
    out.error = error;
    return out;
  }

  static auto offer(std::uint16_t req_id) -> nat::SessionMessage {
    auto out = message(nat::Action::offer, req_id);
    out.conn_request.emplace();
    return out;
  }

  void offered() {
    ASSERT_TRUE(wait_.begin(7, 0x2222));
    wait_.on_console_offer(7, 0x1111);
    ASSERT_EQ(wait_.state(), State::waiting_result);
    ASSERT_EQ(wait_.take_effects().size(), 2U);
  }

  internet::holepunch::OfferWait wait_;
};

TEST_F(HolepunchOfferWait, AcknowledgesAnOfferWithoutARequestId) {
  ASSERT_TRUE(wait_.begin(7, 0x2222));
  wait_.on_console_offer(7, 0);
  EXPECT_EQ(wait_.state(), State::waiting_result);
  auto effects = wait_.take_effects();
  ASSERT_EQ(effects.size(), 2U);
  const auto* ack = std::get_if<AckPeerOffer>(&effects[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->peer_req_id, 0);
}

TEST_F(HolepunchOfferWait, BeginsOneValidAttemptAtATime) {
  EXPECT_FALSE(wait_.begin(0, 2));
  EXPECT_FALSE(wait_.begin(1, 0));
  EXPECT_TRUE(wait_.begin(7, 0x2222));
  EXPECT_FALSE(wait_.begin(8, 0x3333));
}

TEST_F(HolepunchOfferWait, IgnoresAnotherAttemptOrAnOversizedRequestId) {
  ASSERT_TRUE(wait_.begin(7, 0x2222));
  wait_.on_console_offer(8, 0x1111);
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  wait_.on_console_offer(7, 0x10000);
  EXPECT_EQ(wait_.state(), State::waiting_offer);
}

TEST_F(HolepunchOfferWait, AnswersTheConsolesOfferWithOurOwn) {
  ASSERT_TRUE(wait_.begin(7, 0x2222));
  wait_.on_console_offer(7, 0x1111);
  EXPECT_EQ(wait_.state(), State::waiting_result);
  auto effects = wait_.take_effects();
  ASSERT_EQ(effects.size(), 2U);
  const auto* ack = std::get_if<AckPeerOffer>(&effects[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 0x1111);
  EXPECT_EQ(ack->error, 0);
  const auto* own = std::get_if<SendOwnOffer>(&effects[1]);
  ASSERT_TRUE(own);
  EXPECT_EQ(own->attempt, 7U);
  EXPECT_EQ(own->own_req_id, 0x2222);
}

TEST_F(HolepunchOfferWait, OnlyAcknowledgesARepeatedOffer) {
  ASSERT_NO_FATAL_FAILURE(offered());
  wait_.on_console_offer(7, 0x1111);
  auto effects = wait_.take_effects();
  ASSERT_EQ(effects.size(), 1U);
  const auto* ack = std::get_if<AckPeerOffer>(&effects[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 0x1111);
  wait_.on_console_offer(8, 0x1111);
  wait_.on_console_offer(7, 0x10000);
  EXPECT_TRUE(wait_.take_effects().empty());
}

TEST_F(HolepunchOfferWait, IsReadyToProbeOnTheResultForItsOwnOffer) {
  ASSERT_NO_FATAL_FAILURE(offered());
  wait_.on_result(8, 0x2222, 0);
  wait_.on_result(7, 0x9999, 0);
  EXPECT_EQ(wait_.state(), State::waiting_result);
  wait_.on_result(7, 0x2222, 0x80010016);
  EXPECT_EQ(wait_.state(), State::ready_to_probe);
}

TEST_F(HolepunchOfferWait, IgnoresEverythingOnceReadyToProbe) {
  ASSERT_NO_FATAL_FAILURE(offered());
  wait_.on_result(7, 0x2222, 0x80010016);
  ASSERT_EQ(wait_.state(), State::ready_to_probe);
  wait_.on_console_offer(7, 0x1111);
  EXPECT_TRUE(wait_.take_effects().empty());
  wait_.on_result(7, 0x2222, 0);
  EXPECT_FALSE(wait_.timeout(7));
  EXPECT_EQ(wait_.state(), State::ready_to_probe);
}

TEST_F(HolepunchOfferWait, FailsOnACancelOfItsOwnAttempt) {
  ASSERT_TRUE(wait_.begin(3, 9));
  EXPECT_FALSE(wait_.timeout(2));
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  wait_.on_console_offer(3, 10);
  EXPECT_FALSE(wait_.cancel(4));
  EXPECT_EQ(wait_.state(), State::waiting_result);
  EXPECT_EQ(wait_.take_effects().size(), 2U);
  EXPECT_TRUE(wait_.cancel(3));
  EXPECT_EQ(wait_.state(), State::failed);
  EXPECT_TRUE(wait_.take_effects().empty());
  EXPECT_FALSE(wait_.timeout(3));
  EXPECT_FALSE(wait_.begin(4, 11));
  wait_.on_result(3, 9, 0);
  EXPECT_EQ(wait_.state(), State::failed);
}

TEST_F(HolepunchOfferWait, DropsPendingEffectsOnATimeout) {
  ASSERT_TRUE(wait_.begin(5, 12));
  wait_.on_console_offer(5, 13);
  EXPECT_TRUE(wait_.timeout(5));
  EXPECT_TRUE(wait_.take_effects().empty());
}

TEST_F(HolepunchOfferWait, TakesOnlyAnOfferThatCarriesAConnRequest) {
  ASSERT_TRUE(wait_.begin(7, 0x2222));
  wait_.on_message(8, offer(0x1111));
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  wait_.on_message(7, message(nat::Action::offer, 0x1111));
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  wait_.on_message(7, offer(0x1111));
  EXPECT_EQ(wait_.state(), State::waiting_result);
  EXPECT_EQ(wait_.take_effects().size(), 2U);
}

TEST_F(HolepunchOfferWait, TakesTheResultMessageForItsOwnOffer) {
  ASSERT_TRUE(wait_.begin(7, 0x2222));
  wait_.on_message(7, offer(0x1111));
  ASSERT_EQ(wait_.take_effects().size(), 2U);
  wait_.on_message(7, message(nat::Action::result, 0x9999));
  EXPECT_EQ(wait_.state(), State::waiting_result);
  wait_.on_message(7, message(nat::Action::result, 0x2222, 0xffff));
  EXPECT_EQ(wait_.state(), State::ready_to_probe);
}

TEST_F(HolepunchOfferWait, FailsOnATerminateForItsOwnAttempt) {
  ASSERT_TRUE(wait_.begin(3, 9));
  const auto terminate = message(nat::Action::terminate);
  wait_.on_message(4, terminate);
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  wait_.on_message(3, terminate);
  EXPECT_EQ(wait_.state(), State::failed);
}

TEST_F(HolepunchOfferWait, IgnoresAnAcceptMessage) {
  ASSERT_TRUE(wait_.begin(1, 2));
  wait_.on_message(1, message(nat::Action::accept));
  EXPECT_EQ(wait_.state(), State::waiting_offer);
  EXPECT_TRUE(wait_.take_effects().empty());
}

}  // namespace
}  // namespace play_test
