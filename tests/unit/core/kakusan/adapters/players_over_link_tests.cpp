#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"
#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/controller_frame.hpp"
#include "src/core/net/kakusan/link.hpp"
#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/utils/json/strict.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/introductions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::Error;
using libchiaking::net::kakusan::TransportRole;
using test_support::kakusan::Lane;
using test_support::kakusan::LoopbackLink;
using test_support::kakusan::LoopbackTransport;

class PlayersOverLink : public ::testing::Test {
 protected:
  static constexpr SeatBinding seat{2, 5};

  class Vectors {
   public:
    explicit Vectors(libchiaking::platform::CryptoProvider& crypto)
        : introductions(crypto) {
      context.transcript = test_support::Bytes::unhex_array<32>(
          test_support::kakusan::Fixtures::string(vectors, "transcript"));
      context.invitation_id = test_support::Bytes::unhex_array<16>(
          test_support::kakusan::Fixtures::string(vectors, "salt").substr(30));
      guest_context = introductions.for_guest(context);
      context = introductions.for_host(context);
      host_code =
          libchiaking::net::kakusan::Code::parse(
              test_support::kakusan::Fixtures::string(vectors, "host_code"))
              .value();
      guest_code =
          libchiaking::net::kakusan::Code::parse(
              test_support::kakusan::Fixtures::string(vectors, "guest_code"))
              .value();
      for (const auto& step :
           *test_support::kakusan::Fixtures::member(flow, "steps").array()) {
        const auto& item =
            test_support::kakusan::Fixtures::member(step, "message");
        if (test_support::kakusan::Fixtures::string_of(
                test_support::kakusan::Fixtures::member(item, "type")) !=
            "SOURCE_SETUP")
          continue;
        setup = std::get<control::SourceSetup>(
            control::MessageCodec::decode(item.canonical().value()).value());
      }
    }

    std::string vectors{test_support::kakusan::Fixtures::text("vectors.json")};
    libchiaking::json::strict::Value flow{
        libchiaking::json::strict::Value::parse(
            test_support::kakusan::Fixtures::text("control-flow.json"))
            .value()};
    Introductions introductions;
    PairingContext context;
    PairingContext guest_context;
    libchiaking::net::kakusan::Code host_code{
        libchiaking::net::kakusan::Code::from_value(0).value()};
    libchiaking::net::kakusan::Code guest_code{
        libchiaking::net::kakusan::Code::from_value(0).value()};
    control::SourceSetup setup;
  };

  class Rig {
   public:
    Rig(libchiaking::platform::CryptoProvider& provider, const Vectors& given)
        : crypto(provider), vectors(given) {}

    libchiaking::platform::CryptoProvider& crypto;
    const Vectors& vectors;
    std::shared_ptr<LoopbackLink> wire_link{
        std::make_shared<LoopbackLink>(crypto)};
    Link host_link{
        std::make_unique<LoopbackTransport>(wire_link, TransportRole::host),
        TransportRole::host};
    Link guest_link{
        std::make_unique<LoopbackTransport>(wire_link, TransportRole::guest),
        TransportRole::guest};
    test_support::ScriptedRandom host_entropy{0x10};
    test_support::ScriptedRandom guest_entropy{0x90};
    InvitedPlayer host{crypto, host_entropy};
    VisitingPlayer guest{crypto, guest_entropy, vectors.guest_context,
                         vectors.setup.source_protocol};
    std::uint64_t now{1'000'000};
    std::optional<libchiaking::net::kakusan::TransportDescription>
        host_description;
    std::optional<libchiaking::net::kakusan::TransportDescription>
        guest_description;
    std::vector<libchiaking::net::kakusan::InputState> applied;
    std::vector<libchiaking::net::kakusan::FeedbackFrame> rumbles;
    std::vector<std::vector<std::byte>> media;
    std::vector<std::vector<std::byte>> media_acks;
    std::vector<Error> host_malformed;
    std::vector<Error> guest_malformed;
    std::size_t host_media_started{};
    std::size_t guest_media_started{};
    std::size_t host_closed{};
    std::size_t guest_closed{};
    std::optional<Error> host_shown;
    std::optional<Error> guest_shown;
    bool guest_by_peer{};

    void apply(InvitedPlayer::Effects effects) {
      for (auto& effect : effects) {
        if (const auto* send = std::get_if<SendControl>(&effect)) {
          (void)host_link.send_control(send->message, send->token,
                                       send->terminal, now);
        } else if (std::holds_alternative<ReserveSeat>(effect)) {
          apply(host.handle(SeatReserved{now, seat}));
        } else if (std::holds_alternative<RequestSourceJoin>(effect)) {
          apply(host.handle(
              SourceJoined{now, vectors.setup.clone(), 4, 1,
                           libchiaking::net::kakusan::button_mask}));
        } else if (const auto* input = std::get_if<ApplyInput>(&effect)) {
          applied.push_back(input->state);
        } else if (const auto* feedback = std::get_if<SendFeedback>(&effect)) {
          EXPECT_TRUE(host_link.send_feedback(feedback->frame));
        } else if (std::holds_alternative<StartMedia>(effect)) {
          ++host_media_started;
        } else if (std::holds_alternative<CloseTransport>(effect)) {
          ++host_closed;
          host_link.close();
        } else if (const auto* shown = std::get_if<ShowState>(&effect)) {
          if (shown->error) host_shown = shown->error;
        }
      }
    }

    void apply(VisitingPlayer::Effects effects) {
      for (auto& effect : effects) {
        if (const auto* send = std::get_if<SendControl>(&effect)) {
          (void)guest_link.send_control(send->message, send->token,
                                        send->terminal, now);
        } else if (std::holds_alternative<ConfigureReceiver>(effect)) {
          apply(guest.handle(ReceiverReady{now}));
        } else if (std::holds_alternative<PrepareController>(effect)) {
          apply(guest.handle(ControllerReady{now}));
        } else if (const auto* input = std::get_if<SendInput>(&effect)) {
          EXPECT_TRUE(guest_link.send_input(input->frame));
        } else if (std::holds_alternative<StartMediaReception>(effect)) {
          ++guest_media_started;
        } else if (std::holds_alternative<CloseTransport>(effect)) {
          ++guest_closed;
          guest_link.close();
        } else if (const auto* shown = std::get_if<ShowState>(&effect)) {
          if (shown->error) guest_shown = shown->error;
          guest_by_peer = guest_by_peer || shown->by_peer;
        }
      }
    }

    void deliver_host(std::span<LinkEvent> events) {
      for (auto& event : events) {
        if (auto* gathered = std::get_if<link_event::Gathered>(&event))
          host_description = std::move(gathered->local);
        else if (std::holds_alternative<link_event::Nominated>(event))
          apply(host.handle(IceNominated{now}));
        else if (const auto* established =
                     std::get_if<link_event::Established>(&event))
          apply(host.handle(DtlsEstablished{
              now, established->exporter.clone(),
              vectors.introductions.host_proof(vectors.context.transcript,
                                               established->exporter)}));
        else if (auto* message = std::get_if<link_event::Control>(&event))
          apply(host.handle(ControlReceived{now, std::move(message->message)}));
        else if (const auto* acked = std::get_if<link_event::Acked>(&event))
          apply(host.handle(ControlAcked{now, acked->token}));
        else if (const auto* input = std::get_if<link_event::Input>(&event))
          apply(host.handle(InputReceived{now, input->frame}));
        else if (auto* ack = std::get_if<link_event::MediaAck>(&event))
          media_acks.push_back(std::move(ack->record));
        else if (const auto* malformed =
                     std::get_if<link_event::Malformed>(&event)) {
          host_malformed.push_back(malformed->error);
          apply(host.handle(MalformedData{now, malformed->error}));
        } else if (std::holds_alternative<link_event::Failed>(event))
          apply(host.handle(TransportFailed{now}));
        else
          ADD_FAILURE() << "unexpected link event";
      }
    }

    void deliver_guest(std::span<LinkEvent> events) {
      for (auto& event : events) {
        if (auto* gathered = std::get_if<link_event::Gathered>(&event))
          guest_description = std::move(gathered->local);
        else if (std::holds_alternative<link_event::Nominated>(event))
          apply(guest.handle(IceNominated{now}));
        else if (const auto* established =
                     std::get_if<link_event::Established>(&event))
          apply(guest.handle(DtlsEstablished{
              now, established->exporter.clone(),
              vectors.introductions.guest_proof(vectors.context.transcript,
                                                established->exporter)}));
        else if (auto* message = std::get_if<link_event::Control>(&event))
          apply(
              guest.handle(ControlReceived{now, std::move(message->message)}));
        else if (const auto* acked = std::get_if<link_event::Acked>(&event))
          apply(guest.handle(ControlAcked{now, acked->token}));
        else if (const auto* feedback =
                     std::get_if<link_event::Feedback>(&event)) {
          rumbles.push_back(feedback->frame);
          apply(guest.handle(FeedbackReceived{now, feedback->frame}));
        } else if (auto* datagram = std::get_if<link_event::Media>(&event))
          media.push_back(std::move(datagram->datagram));
        else if (const auto* malformed =
                     std::get_if<link_event::Malformed>(&event)) {
          guest_malformed.push_back(malformed->error);
          apply(guest.handle(MalformedData{now, malformed->error}));
        } else if (std::holds_alternative<link_event::Failed>(event))
          apply(guest.handle(TransportFailed{now}));
        else
          ADD_FAILURE() << "unexpected link event";
      }
    }

    void step(std::uint64_t advance_us = 1'000) {
      now += advance_us;
      auto events = host_link.pump(now);
      deliver_host(events);
      apply(host.on_time(now));
      events = guest_link.pump(now);
      deliver_guest(events);
      apply(guest.on_time(now));
    }

    void run(std::size_t steps, std::uint64_t advance_us = 1'000) {
      for (std::size_t index = 0; index < steps; ++index) step(advance_us);
    }

    void connect() {
      ASSERT_TRUE(host_link.gather(now, std::nullopt));
      ASSERT_TRUE(guest_link.gather(now, std::nullopt));
      step();
      ASSERT_TRUE(host_description);
      ASSERT_TRUE(guest_description);
      apply(host.handle(ReplyImported{now, vectors.context}));
      apply(host.handle(AdmissionApproved{now}));
      ASSERT_TRUE(host_link.connect(*guest_description,
                                    vectors.context.transcript, now));
      ASSERT_TRUE(guest_link.connect(*host_description,
                                     vectors.context.transcript, now));
      for (std::size_t index = 0;
           index < 5000 && (host.state() != Progress::verifying ||
                            guest.state() != Progress::verifying);
           ++index)
        step();
      ASSERT_EQ(host.state(), Progress::verifying);
      ASSERT_EQ(guest.state(), Progress::verifying);
    }

    void pair() {
      apply(guest.handle(LocalCode{
          now, libchiaking::net::kakusan::Code::from_value(7306).value()}));
      apply(guest.handle(PeerCodeEntered{now, vectors.host_code.clone()}));
      apply(host.handle(LocalCode{
          now, libchiaking::net::kakusan::Code::from_value(4821).value()}));
      apply(host.handle(PeerCodeEntered{now, vectors.guest_code.clone()}));
      run(200);
    }
  };

  test_support::SoftwareCrypto crypto;
  Vectors vectors{crypto};
  Rig rig{crypto, vectors};
};

class PlayersOverAConnectedLink : public PlayersOverLink {
 protected:
  void SetUp() override { ASSERT_NO_FATAL_FAILURE(rig.connect()); }
};

class PlayersOverAPairedLink : public PlayersOverAConnectedLink {
 protected:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(PlayersOverAConnectedLink::SetUp());
    rig.pair();
  }
};

TEST_F(PlayersOverAConnectedLink, EstablishBothEnds) {
  EXPECT_TRUE(rig.host_link.established());
  EXPECT_TRUE(rig.guest_link.established());
}

TEST_F(PlayersOverAPairedLink, PlayWithTheSameMediaKey) {
  EXPECT_EQ(rig.host.state(), Progress::playing);
  EXPECT_EQ(rig.guest.state(), Progress::playing);
  EXPECT_EQ(rig.host_media_started, 1U);
  EXPECT_EQ(rig.guest_media_started, 1U);
  EXPECT_FALSE(rig.applied.empty());

  const auto host_key = rig.host_link.media_key();
  const auto guest_key = rig.guest_link.media_key();
  ASSERT_TRUE(host_key);
  ASSERT_TRUE(guest_key);
  EXPECT_TRUE(std::ranges::equal(host_key.value(), guest_key.value()));
}

TEST_F(PlayersOverAPairedLink, CarryInputAndRumble) {
  const auto south =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::south);
  rig.apply(rig.guest.handle(
      ControllerSampled{rig.now, InputState{south, 0, 0, 0, 0, 0, 0, {}}}));
  rig.apply(rig.host.handle(RumbleSampled{rig.now, 900, 300}));
  rig.run(100);
  ASSERT_FALSE(rig.applied.empty());
  EXPECT_EQ(rig.applied.back().buttons, south);
  ASSERT_FALSE(rig.rumbles.empty());
  EXPECT_EQ(rig.rumbles.back().low_frequency, 900);
}

TEST_F(PlayersOverAPairedLink, CarryMediaToTheGuestAndAcksToTheHost) {
  const std::vector<std::byte> datagram(76, std::byte{0x5a});
  EXPECT_TRUE(rig.host_link.send_media(datagram));
  EXPECT_FALSE(rig.guest_link.send_media(datagram));
  const std::vector<std::byte> ack{std::byte{'R'}, std::byte{'C'},
                                   std::byte{'A'}, std::byte{'1'},
                                   std::byte{1}};
  EXPECT_TRUE(rig.guest_link.send_media_ack(ack));
  EXPECT_FALSE(rig.host_link.send_media_ack(ack));
  rig.run(3);
  ASSERT_EQ(rig.media.size(), 1U);
  EXPECT_EQ(rig.media.front(), datagram);
  ASSERT_EQ(rig.media_acks.size(), 1U);
  EXPECT_EQ(rig.media_acks.front(), ack);
}

TEST_F(PlayersOverAPairedLink, KeepPlayingAndCloseInTurnWhenTheGuestCancels) {
  rig.run(3000);
  EXPECT_EQ(rig.host.state(), Progress::playing);
  EXPECT_EQ(rig.guest.state(), Progress::playing);

  rig.apply(rig.guest.handle(Cancel{rig.now, control::Reason::user}));
  rig.run(50);
  EXPECT_EQ(rig.guest.state(), Progress::gone);
  EXPECT_EQ(rig.guest_closed, 1U);
  EXPECT_TRUE(rig.guest_link.closed());
  EXPECT_EQ(rig.host.state(), Progress::leaving);
  rig.run(1100);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.host_closed, 1U);
  EXPECT_TRUE(rig.host_link.closed());
  EXPECT_TRUE(rig.host_malformed.empty());
  EXPECT_TRUE(rig.guest_malformed.empty());
  EXPECT_FALSE(rig.host_link.next_deadline_us());
}

TEST_F(PlayersOverLink, PairDespiteLosingEveryThirdControlRecord) {
  std::size_t counter = 0;
  rig.wire_link->drop = [&counter](TransportRole, Lane lane,
                                   const std::vector<std::byte>&) {
    return lane == Lane::record && ++counter % 3 == 0;
  };
  ASSERT_NO_FATAL_FAILURE(rig.connect());
  rig.pair();
  rig.run(6000);
  EXPECT_EQ(rig.host.state(), Progress::playing);
  EXPECT_EQ(rig.guest.state(), Progress::playing);
}

TEST_F(PlayersOverAPairedLink, EndOnAnUnknownRecord) {
  EXPECT_EQ(rig.guest.state(), Progress::playing);
  auto raw =
      std::make_unique<LoopbackTransport>(rig.wire_link, TransportRole::guest);
  const std::vector<std::byte> bogus{std::byte{'R'}, std::byte{'C'},
                                     std::byte{'X'}, std::byte{'9'},
                                     std::byte{0}};
  EXPECT_TRUE(raw->send_record(bogus));
  rig.run(50);
  ASSERT_EQ(rig.host_malformed.size(), 1U);
  EXPECT_EQ(rig.host_malformed.front(), Error::protocol);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.guest_shown, Error::protocol);
  EXPECT_TRUE(rig.guest_by_peer);
}

TEST_F(PlayersOverAPairedLink, EndOnFeedbackFromTheGuest) {
  auto raw =
      std::make_unique<LoopbackTransport>(rig.wire_link, TransportRole::guest);
  const auto frame =
      ControllerFrameCodec::encode_feedback(FeedbackFrame{seat, 1, 1, 0, 0});
  ASSERT_TRUE(frame);
  EXPECT_TRUE(raw->send_record(frame.value()));
  rig.run(50);
  EXPECT_EQ(rig.host_malformed.size(), 1U);
  EXPECT_EQ(rig.host.state(), Progress::gone);
}

TEST_F(PlayersOverAConnectedLink, EndWhenThePeerFallsSilent) {
  rig.wire_link->drop = [](TransportRole role, Lane,
                           const std::vector<std::byte>&) {
    return role == TransportRole::guest;
  };
  rig.apply(rig.host.handle(LocalCode{
      rig.now, libchiaking::net::kakusan::Code::from_value(4821).value()}));
  rig.apply(
      rig.host.handle(PeerCodeEntered{rig.now, vectors.guest_code.clone()}));
  rig.run(400, 50'000);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.host_closed, 1U);
}

TEST_F(PlayersOverLink, BothEndWhenTheTransportFails) {
  rig.wire_link->reject_certificates = true;
  ASSERT_TRUE(rig.host_link.gather(rig.now, std::nullopt));
  ASSERT_TRUE(rig.guest_link.gather(rig.now, std::nullopt));
  rig.step();
  rig.apply(rig.host.handle(ReplyImported{rig.now, vectors.context}));
  ASSERT_TRUE(rig.guest_description);
  ASSERT_TRUE(rig.host_description);
  EXPECT_TRUE(rig.host_link.connect(*rig.guest_description,
                                    vectors.context.transcript, rig.now));
  EXPECT_TRUE(rig.guest_link.connect(*rig.host_description,
                                     vectors.context.transcript, rig.now));
  rig.run(5);
  EXPECT_EQ(rig.host.state(), Progress::gone);
  EXPECT_EQ(rig.guest.state(), Progress::gone);
  EXPECT_FALSE(rig.host_link.established());
  EXPECT_FALSE(rig.host_link.media_key());
}

}  // namespace
}  // namespace kakusan_test
