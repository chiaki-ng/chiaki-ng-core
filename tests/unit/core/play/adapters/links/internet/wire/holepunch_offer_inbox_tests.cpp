#include "src/core/net/nat/holepunch/holepunch_codec.hpp"
#include "src/core/play/adapters/links/internet/wire/holepunch_offer.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/psn/notifications.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string_view>
#include <utility>
#include <variant>

namespace play_test {
namespace {

namespace nat = libchiaking::net::nat::holepunch;
namespace internet = libchiaking::play::internet;
using internet::holepunch::Offer;
using internet::holepunch::OfferWaitState;

class HolepunchOfferInbox : public ::testing::Test {
 protected:
  static auto offer(std::uint16_t req_id, std::uint32_t sid)
      -> nat::SessionMessage {
    nat::SessionMessage message{};
    message.action = nat::Action::offer;
    message.req_id = req_id;
    message.conn_request.emplace();
    message.conn_request->sid = sid;
    return message;
  }

  static auto result(std::uint16_t req_id, std::uint16_t error = 0)
      -> nat::SessionMessage {
    nat::SessionMessage message{};
    message.action = nat::Action::result;
    message.req_id = req_id;
    message.error = error;
    return message;
  }

  void offered() {
    ASSERT_TRUE(inbox_.begin(7, 0x2222));
    inbox_.on_message(7, offer(0x1111, 11));
    ASSERT_EQ(inbox_.state(), OfferWaitState::waiting_result);
  }

  internet::holepunch::OfferInbox inbox_;
};

TEST_F(HolepunchOfferInbox, WaitsForAnOfferOfItsOwnAttemptWithAConnRequest) {
  ASSERT_TRUE(inbox_.begin(7, 0x2222));
  inbox_.on_message(8, offer(0x1111, 8));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_offer);
  nat::SessionMessage bare{};
  bare.action = nat::Action::offer;
  bare.req_id = 0x1111;
  inbox_.on_message(7, std::move(bare));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_offer);
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
}

TEST_F(HolepunchOfferInbox, AnswersTheOfferButHoldsThePeerRequest) {
  ASSERT_NO_FATAL_FAILURE(offered());
  EXPECT_FALSE(inbox_.begin(9, 0x3333));
  auto effects = inbox_.take_effects();
  EXPECT_EQ(effects.size(), 2U);
  EXPECT_TRUE(inbox_.take_effects().empty());
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
}

TEST_F(HolepunchOfferInbox, HandsOverTheFirstPeerRequestOnItsResult) {
  ASSERT_NO_FATAL_FAILURE(offered());
  ASSERT_EQ(inbox_.take_effects().size(), 2U);
  inbox_.on_message(7, offer(0x1112, 12));
  inbox_.on_message(8, offer(0x1113, 13));
  inbox_.on_message(7, result(0x9999));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_result);
  inbox_.on_message(7, result(0x2222, 0xffff));
  EXPECT_EQ(inbox_.state(), OfferWaitState::ready_to_probe);
  auto request = inbox_.take_peer_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->sid, 11U);
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
}

TEST_F(HolepunchOfferInbox, DropsThePeerRequestWhenCancelled) {
  ASSERT_TRUE(inbox_.begin(3, 9));
  inbox_.on_message(3, offer(10, 21));
  EXPECT_FALSE(inbox_.cancel(4));
  EXPECT_TRUE(inbox_.cancel(3));
  EXPECT_EQ(inbox_.state(), OfferWaitState::failed);
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
  EXPECT_TRUE(inbox_.take_effects().empty());
}

TEST_F(HolepunchOfferInbox, DropsThePeerRequestWhenTerminated) {
  ASSERT_TRUE(inbox_.begin(5, 19));
  inbox_.on_message(5, offer(20, 31));
  nat::SessionMessage terminate{};
  terminate.action = nat::Action::terminate;
  inbox_.on_message(5, std::move(terminate));
  EXPECT_EQ(inbox_.state(), OfferWaitState::failed);
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
  EXPECT_TRUE(inbox_.take_effects().empty());
}

TEST_F(HolepunchOfferInbox, DropsThePeerRequestWhenTimedOut) {
  ASSERT_TRUE(inbox_.begin(6, 29));
  inbox_.on_message(6, offer(30, 41));
  EXPECT_TRUE(inbox_.timeout(6));
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
  EXPECT_TRUE(inbox_.take_effects().empty());
}

class HolepunchOfferFeed : public ::testing::Test {
 protected:
  static constexpr std::string_view offer_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"OFFER\",\"reqId\":2,\"error\":0,\"connRequest\":{\"sid\":3,\"peerSid\":4,\"skey\":\"AAAAAAAAAAAAAAAAAAAAAA==\",\"natType\":2,\"candidate\":[],\"defaultRouteMacAddr\":\"\",\"localPeerAddr\":,\"localHashedId\":\"AAAAAAAAAAAAAAAAAAAAAAAAAAA=\"}}"}}}})";
  static constexpr std::string_view result_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"ver=1.0, type=text, body={\"action\":\"RESULT\",\"reqId\":1,\"error\":0,\"connRequest\":{}}"}}}})";
  static constexpr std::string_view malformed_json =
      R"({"body":{"data":{"sessionMessage":{"payload":"body=not-json"}}}})";

  auto feed(std::uint64_t attempt, std::string_view json) {
    return Offer::feed_inbox(
        inbox_, attempt,
        test_support::psn::Notifications::typed(
            libchiaking::psn::rest::NotificationType::session_message_created,
            json));
  }

  void offered() {
    ASSERT_TRUE(inbox_.begin(7, 1));
    ASSERT_TRUE(feed(7, offer_json));
    ASSERT_EQ(inbox_.take_effects().size(), 2U);
  }

  internet::holepunch::OfferInbox inbox_;
};

TEST_F(HolepunchOfferFeed, IgnoresAnotherAttemptsOffer) {
  ASSERT_TRUE(inbox_.begin(7, 1));
  EXPECT_TRUE(feed(8, offer_json));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_offer);
  EXPECT_TRUE(inbox_.take_effects().empty());
}

TEST_F(HolepunchOfferFeed, AnswersItsOwnAttemptsOffer) {
  ASSERT_TRUE(inbox_.begin(7, 1));
  EXPECT_TRUE(feed(7, offer_json));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_result);
  EXPECT_EQ(inbox_.take_effects().size(), 2U);
}

TEST_F(HolepunchOfferFeed, OnlyAcknowledgesARepeatedOffer) {
  ASSERT_NO_FATAL_FAILURE(offered());
  EXPECT_TRUE(feed(7, offer_json));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_result);
  auto effects = inbox_.take_effects();
  ASSERT_EQ(effects.size(), 1U);
  const auto* ack = std::get_if<internet::holepunch::AckPeerOffer>(&effects[0]);
  ASSERT_TRUE(ack);
  EXPECT_EQ(ack->attempt, 7U);
  EXPECT_EQ(ack->peer_req_id, 2);
}

TEST_F(HolepunchOfferFeed, RejectsAMalformedMessageAndKeepsWaiting) {
  ASSERT_NO_FATAL_FAILURE(offered());
  auto malformed = feed(7, malformed_json);
  ASSERT_FALSE(malformed.has_value());
  EXPECT_TRUE(std::holds_alternative<nat::MessageError>(malformed.error()));
  EXPECT_EQ(inbox_.state(), OfferWaitState::waiting_result);
}

TEST_F(HolepunchOfferFeed, HandsOverThePeerRequestOnTheResult) {
  ASSERT_NO_FATAL_FAILURE(offered());
  EXPECT_TRUE(feed(7, result_json));
  EXPECT_EQ(inbox_.state(), OfferWaitState::ready_to_probe);
  auto peer = inbox_.take_peer_request();
  ASSERT_TRUE(peer.has_value());
  EXPECT_EQ(peer->sid, 3U);
  EXPECT_FALSE(inbox_.take_peer_request().has_value());
}

}  // namespace
}  // namespace play_test
