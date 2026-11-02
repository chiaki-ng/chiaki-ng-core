#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"
#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/net/kakusan/transport_frame.hpp"
#include "src/core/utils/json/strict.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/bytes.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/fixtures.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/effect_snapshot.hpp"
#include "tests/unit/core/kakusan/introductions.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {
namespace {

using namespace libchiaking::kakusan;
namespace control = libchiaking::net::kakusan::control;
namespace wire = libchiaking::net::kakusan;
using libchiaking::net::kakusan::Exporter;
using test_support::Bytes;
using test_support::kakusan::Fixtures;

class PlayerPair : public ::testing::Test {
 protected:
  enum class Sender : std::uint8_t {
    host,
    guest,
  };

  struct Sent {
    Sender sender{Sender::host};
    std::string text;
  };

  static constexpr wire::SeatBinding seat{3, 7};
  static constexpr std::uint32_t south =
      static_cast<std::uint32_t>(wire::Button::south);

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  class Vectors {
   public:
    Vectors() {
      context.transcript =
          Bytes::unhex_array<32>(Fixtures::string(document, "transcript"));
      context.invitation_id =
          Bytes::unhex_array<16>(Fixtures::string(document, "salt").substr(30));
      guest_context = introductions.for_guest(context);
      context = introductions.for_host(context);
      std::ranges::copy(Bytes::unhex_array<32>(
                            Fixtures::string(document, "pairing_exporter")),
                        exporter.begin());
      std::ranges::copy(Bytes::unhex_array<32>(Fixtures::string(document, "x")),
                        host_scalar.begin());
      std::ranges::copy(Bytes::unhex_array<32>(Fixtures::string(document, "y")),
                        guest_scalar.begin());
      host_code =
          wire::Code::parse(Fixtures::string(document, "host_code")).value();
      guest_code =
          wire::Code::parse(Fixtures::string(document, "guest_code")).value();
      for (const auto& step : *Fixtures::member(flow, "steps").array()) {
        const auto& item = Fixtures::member(step, "message");
        if (Fixtures::string_of(Fixtures::member(item, "type")) !=
            "SOURCE_SETUP")
          continue;
        setup = std::get<control::SourceSetup>(
            control::MessageCodec::decode(item.canonical().value()).value());
      }
    }

    std::string document{Fixtures::text("vectors.json")};
    libchiaking::json::strict::Value flow{
        libchiaking::json::strict::Value::parse(
            Fixtures::text("control-flow.json"))
            .value()};
    Introductions introductions{crypto()};
    PairingContext context;
    PairingContext guest_context;
    Exporter exporter{};
    wire::Scalar host_scalar{};
    wire::Scalar guest_scalar{};
    wire::Code host_code{wire::Code::from_value(0).value()};
    wire::Code guest_code{wire::Code::from_value(0).value()};
    control::SourceSetup setup;
  };

  static auto vectors() -> const Vectors& {
    static const Vectors shared;
    return shared;
  }

  static auto wide_of(const wire::Scalar& scalar) -> std::vector<std::byte> {
    std::vector<std::byte> wide(wire::wide_scalar_size);
    std::copy(scalar.begin(), scalar.end(), wide.begin() + 32);
    return wide;
  }

  template <class T, class Effects>
  static auto count(const Effects& effects) -> std::size_t {
    return static_cast<std::size_t>(std::ranges::count_if(
        effects,
        [](const auto& effect) { return std::holds_alternative<T>(effect); }));
  }

  class Pair {
   public:
    Pair()
        : host(crypto(), host_entropy),
          guest(crypto(), guest_entropy, vectors().guest_context,
                vectors().setup.source_protocol) {
      host_entropy.queue.push_back(wide_of(vectors().host_scalar));
      guest_entropy.queue.push_back(wide_of(vectors().guest_scalar));
    }

    void to_host(InvitedPlayer::Event event) {
      absorb(host.handle(std::move(event)));
    }

    void to_guest(VisitingPlayer::Event event) {
      absorb(guest.handle(std::move(event)));
    }

    void absorb(InvitedPlayer::Effects effects) {
      for (auto& effect : effects) {
        host_log.push_back(Effects::snapshot(effect));
        if (const auto* send = std::get_if<SendControl>(&effect)) {
          auto text =
              std::string{control::MessageCodec::encode_secret(send->message)
                              .value()
                              .view()};
          controls.push_back(Sent{Sender::host, text});
          const auto token = send->token;
          work.emplace_back([this, text, token] {
            to_guest(ControlReceived{
                now, control::MessageCodec::decode(text).value()});
            to_host(ControlAcked{now, token});
          });
        } else if (std::holds_alternative<ReserveSeat>(effect)) {
          work.emplace_back([this] { to_host(SeatReserved{now, seat}); });
        } else if (std::holds_alternative<RequestSourceJoin>(effect)) {
          work.emplace_back([this] {
            to_host(SourceJoined{now, vectors().setup.clone(), 4, 2,
                                 wire::button_mask});
          });
        } else if (const auto* feedback = std::get_if<SendFeedback>(&effect)) {
          const auto bytes =
              wire::ControllerFrameCodec::encode_feedback(feedback->frame)
                  .value();
          work.emplace_back([this, bytes] {
            const auto frame =
                wire::ControllerFrameCodec::decode_feedback(bytes).value();
            delivered_feedback.push_back(frame);
            to_guest(FeedbackReceived{now, frame});
          });
        } else if (const auto* applied = std::get_if<ApplyInput>(&effect)) {
          EXPECT_EQ(applied->binding, seat);
          applied_states.push_back(applied->state);
        }
      }
    }

    void absorb(VisitingPlayer::Effects effects) {
      for (auto& effect : effects) {
        guest_log.push_back(Effects::snapshot(effect));
        if (const auto* send = std::get_if<SendControl>(&effect)) {
          auto text =
              std::string{control::MessageCodec::encode_secret(send->message)
                              .value()
                              .view()};
          controls.push_back(Sent{Sender::guest, text});
          const auto token = send->token;
          work.emplace_back([this, text, token] {
            to_host(ControlReceived{
                now, control::MessageCodec::decode(text).value()});
            to_guest(ControlAcked{now, token});
          });
        } else if (const auto* configure =
                       std::get_if<ConfigureReceiver>(&effect)) {
          EXPECT_EQ(
              control::MessageCodec::setup_hash(crypto(), configure->setup)
                  .value(),
              control::MessageCodec::setup_hash(crypto(), vectors().setup)
                  .value());
          work.emplace_back([this] { to_guest(ReceiverReady{now}); });
        } else if (const auto* prepare =
                       std::get_if<PrepareController>(&effect)) {
          EXPECT_EQ(prepare->grant.binding, seat);
          work.emplace_back([this] { to_guest(ControllerReady{now}); });
        } else if (const auto* input = std::get_if<SendInput>(&effect)) {
          const auto bytes =
              wire::ControllerFrameCodec::encode_input(input->frame).value();
          work.emplace_back([this, bytes] {
            const auto frame =
                wire::ControllerFrameCodec::decode_input(bytes).value();
            applied_inputs.push_back(frame);
            to_host(InputReceived{now, frame});
          });
        }
      }
    }

    void run() {
      while (!work.empty()) {
        auto item = std::move(work.front());
        work.pop_front();
        item();
      }
    }

    void tick(std::uint64_t time) {
      now = time;
      absorb(host.on_time(time));
      absorb(guest.on_time(time));
      run();
    }

    void connect(const Exporter& host_exporter, const Exporter& guest_exporter,
                 bool approve = true) {
      to_host(ReplyImported{now, vectors().context});
      if (approve) to_host(AdmissionApproved{now});
      to_guest(IceNominated{now});
      to_host(IceNominated{now});
      to_host(
          DtlsEstablished{now, host_exporter.clone(),
                          vectors().introductions.host_proof(
                              vectors().context.transcript, host_exporter)});
      to_guest(
          DtlsEstablished{now, guest_exporter.clone(),
                          vectors().introductions.guest_proof(
                              vectors().context.transcript, guest_exporter)});
      run();
    }

    void connect() { connect(vectors().exporter, vectors().exporter); }

    void exchange_codes(const wire::Code& entered_by_guest) {
      to_guest(LocalCode{now, wire::Code::from_value(7306).value()});
      to_guest(PeerCodeEntered{now, entered_by_guest.clone()});
      to_host(LocalCode{now, wire::Code::from_value(4821).value()});
      to_host(PeerCodeEntered{now, vectors().guest_code.clone()});
      run();
    }

    void play() {
      connect();
      now += 30'000'000;
      exchange_codes(vectors().host_code);
    }

    void exercise_controllers() {
      const auto active_at = now;
      for (std::uint64_t step = 1; step <= 600; ++step) {
        now = active_at + step * 1'000;
        if (step == 20)
          to_guest(ControllerSampled{
              now, wire::InputState{south, 0, 0, 0, 0, 0, 0, {}}});
        if (step == 120) to_guest(ControllerSampled{now, wire::InputState{}});
        if (step == 50) to_host(RumbleSampled{now, 1000, 500});
        if (step == 300) to_host(RumbleSampled{now, 0, 0});
        tick(now);
      }
    }

    test_support::ScriptedRandom host_entropy{0x20};
    test_support::ScriptedRandom guest_entropy{0x20};
    InvitedPlayer host;
    VisitingPlayer guest;
    std::uint64_t now{5'000'000};
    std::vector<Sent> controls;
    std::deque<std::function<void()>> work;
    InvitedPlayer::Effects host_log;
    VisitingPlayer::Effects guest_log;
    std::vector<wire::InputFrame> applied_inputs;
    std::vector<wire::InputState> applied_states;
    std::vector<wire::FeedbackFrame> delivered_feedback;
  };

  static void expect_fixture_order(const Pair& pair) {
    const auto& steps = *Fixtures::member(vectors().flow, "steps").array();
    ASSERT_EQ(pair.controls.size(), steps.size());
    std::uint32_t host_id = 0;
    std::uint32_t guest_id = 0;
    for (std::size_t index = 0; index < steps.size(); ++index) {
      SCOPED_TRACE(index);
      const auto& step = steps[index];
      const auto& sent = pair.controls[index];
      const auto expected_sender =
          Fixtures::string_of(Fixtures::member(step, "sender")) == "host"
              ? Sender::host
              : Sender::guest;
      EXPECT_EQ(sent.sender, expected_sender);
      EXPECT_EQ(sent.text,
                Fixtures::member(step, "message").canonical().value());
      const auto id = sent.sender == Sender::host ? ++host_id : ++guest_id;
      EXPECT_EQ(id, *Fixtures::member(step, "control_id").integer());
      const auto fragments =
          wire::TransportFrameCodec::encode_control_fragments(
              id, wire::Hash::bytes_of(sent.text));
      ASSERT_TRUE(fragments);
      const auto& expected =
          *Fixtures::member(step, "dtls_plaintext_hex").array();
      ASSERT_EQ(fragments.value().size(), expected.size());
      for (std::size_t part = 0; part < expected.size(); ++part)
        EXPECT_EQ(Bytes::hex(fragments.value()[part]),
                  Fixtures::string_of(expected[part]));
    }
  }
};

TEST_F(PlayerPair, WaitForCodesOnceTheChannelIsProven) {
  Pair pair;
  pair.connect();
  EXPECT_EQ(pair.host.state(), Progress::verifying);
  EXPECT_EQ(pair.guest.state(), Progress::verifying);
}

TEST_F(PlayerPair, PlayOnOneSeatOnceBothCodesMatch) {
  Pair pair;
  pair.play();
  EXPECT_EQ(pair.host.state(), Progress::playing);
  EXPECT_EQ(pair.guest.state(), Progress::playing);
  EXPECT_EQ(pair.host.binding(), seat);
  EXPECT_EQ(pair.guest.binding(), seat);
  EXPECT_EQ(count<StartMedia>(pair.host_log), 1U);
  EXPECT_EQ(count<StartMediaReception>(pair.guest_log), 1U);
  EXPECT_EQ(count<GrantSeat>(pair.host_log), 1U);
  EXPECT_EQ(pair.applied_states.size(), 1U);
  ASSERT_FALSE(pair.applied_inputs.empty());
  EXPECT_EQ(pair.applied_inputs.front().sequence, 0U);
}

TEST_F(PlayerPair, CarryInputFramesInSequenceWhilePlaying) {
  Pair pair;
  pair.play();
  pair.exercise_controllers();
  EXPECT_GE(pair.applied_inputs.size(), 35U);
  EXPECT_LE(pair.applied_inputs.size(), 45U);
  for (std::size_t index = 1; index < pair.applied_inputs.size(); ++index)
    EXPECT_EQ(pair.applied_inputs[index].sequence,
              pair.applied_inputs[index - 1].sequence + 1);
  EXPECT_EQ(pair.applied_states.size(), pair.applied_inputs.size());
  EXPECT_TRUE(std::ranges::any_of(
      pair.applied_states,
      [](const wire::InputState& state) { return state.buttons == south; }));
  ASSERT_FALSE(pair.applied_states.empty());
  EXPECT_EQ(pair.applied_states.back().buttons, 0U);
  ASSERT_FALSE(pair.applied_inputs.empty());
  EXPECT_GE(pair.applied_inputs.back().elapsed_ms, 580U);
  EXPECT_EQ(count<InjectNeutral>(pair.host_log), 0U);
}

TEST_F(PlayerPair, CarryRumbleUntilItStops) {
  Pair pair;
  pair.play();
  pair.exercise_controllers();
  ASSERT_FALSE(pair.delivered_feedback.empty());
  EXPECT_EQ(pair.delivered_feedback.front().low_frequency, 1000U);
  EXPECT_EQ(pair.delivered_feedback.front().high_frequency, 500U);
  EXPECT_EQ(pair.delivered_feedback.back().low_frequency, 0U);
  EXPECT_GE(pair.delivered_feedback.size(), 15U);
  EXPECT_LE(pair.delivered_feedback.size(), 18U);
  EXPECT_EQ(pair.delivered_feedback.back().elapsed_ms, 300U);
  EXPECT_EQ(count<ApplyRumble>(pair.guest_log), pair.delivered_feedback.size());
  EXPECT_EQ(count<StopRumble>(pair.guest_log), 0U);
}

TEST_F(PlayerPair, PassTheGuestsIdrRequestToTheHost) {
  Pair pair;
  pair.play();
  pair.exercise_controllers();
  pair.to_guest(IdrNeeded{pair.now});
  pair.run();
  EXPECT_EQ(count<RequestIdr>(pair.host_log), 1U);
}

TEST_F(PlayerPair, FollowTheFixtureControlFlowThroughALeave) {
  Pair pair;
  pair.play();
  pair.exercise_controllers();
  pair.to_guest(IdrNeeded{pair.now});
  pair.run();
  pair.to_guest(Cancel{pair.now, control::Reason::user});
  pair.run();
  expect_fixture_order(pair);
  EXPECT_EQ(pair.controls.size(), 18U);
}

TEST_F(PlayerPair, TearDownBothSidesWhenTheGuestLeaves) {
  Pair pair;
  pair.play();
  pair.exercise_controllers();
  pair.to_guest(IdrNeeded{pair.now});
  pair.run();
  pair.to_guest(Cancel{pair.now, control::Reason::user});
  pair.run();

  EXPECT_EQ(pair.guest.state(), Progress::gone);
  EXPECT_EQ(count<CloseTransport>(pair.guest_log), 1U);
  EXPECT_EQ(count<StopRumble>(pair.guest_log), 1U);
  EXPECT_EQ(count<StopMediaReception>(pair.guest_log), 1U);
  EXPECT_EQ(count<EraseSecrets>(pair.guest_log), 1U);
  EXPECT_EQ(count<ConsumeInvitation>(pair.guest_log), 1U);

  EXPECT_EQ(pair.host.state(), Progress::leaving);
  EXPECT_EQ(count<StopMedia>(pair.host_log), 1U);
  EXPECT_EQ(count<InjectNeutral>(pair.host_log), 1U);
  EXPECT_EQ(count<RequestSourceRemoval>(pair.host_log), 1U);
  EXPECT_EQ(count<ReleaseSeat>(pair.host_log), 1U);
  EXPECT_EQ(count<EraseSecrets>(pair.host_log), 1U);
  const auto left = pair.now;
  pair.tick(left + terminal_grace_us - 1);
  EXPECT_EQ(pair.host.state(), Progress::leaving);
  pair.tick(left + terminal_grace_us);
  EXPECT_EQ(pair.host.state(), Progress::gone);
  EXPECT_EQ(count<CloseTransport>(pair.host_log), 1U);
  EXPECT_EQ(count<ConsumeInvitation>(pair.host_log), 1U);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.host_log), 0U);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.guest_log), 0U);
}

TEST_F(PlayerPair, FailPairingOnBothSidesWhenTheCodesDiffer) {
  Pair pair;
  pair.connect();
  pair.exchange_codes(wire::Code::parse("4820").value());
  EXPECT_EQ(pair.guest.state(), Progress::gone);
  EXPECT_EQ(pair.host.state(), Progress::leaving);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.guest_log), 1U);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.host_log), 1U);
  EXPECT_EQ(count<StartMedia>(pair.host_log), 0U);
  EXPECT_EQ(count<ConfigureReceiver>(pair.guest_log), 0U);
  ASSERT_EQ(pair.controls.size(), 8U);
  EXPECT_EQ(pair.controls.back().sender, Sender::guest);
  EXPECT_EQ(pair.controls.back().text,
            "{\"code\":\"PAIRING_FAILED\",\"type\":\"ERROR\"}");
}

TEST_F(PlayerPair, EndWithoutASeatWhenTheChannelsDiffer) {
  Pair pair;
  auto exporter = vectors().exporter.clone();
  exporter[0] ^= std::byte{1};
  pair.connect(vectors().exporter, exporter, false);
  pair.exchange_codes(vectors().host_code);
  EXPECT_EQ(pair.guest.state(), Progress::gone);
  EXPECT_TRUE(pair.host.state() == Progress::gone ||
              pair.host.state() == Progress::leaving)
      << "host state " << static_cast<int>(pair.host.state());
  ASSERT_FALSE(pair.controls.empty());
  EXPECT_EQ(pair.controls.back().text,
            "{\"code\":\"IDENTITY_CHANGED\",\"type\":\"ERROR\"}");
  EXPECT_TRUE(count<DisplayCode>(pair.host_log) == 0 ||
              count<DisplayCode>(pair.guest_log) == 0);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.host_log), 0U);
  EXPECT_EQ(count<RecordAuthenticationFailure>(pair.guest_log), 0U);
  EXPECT_EQ(count<ReserveSeat>(pair.host_log), 0U);
}

TEST_F(PlayerPair, ReportAHostFailureToTheGuestOnce) {
  Pair pair;
  pair.connect();
  pair.exchange_codes(vectors().host_code);
  EXPECT_EQ(pair.host.state(), Progress::playing);

  pair.to_host(LocalFailure{pair.now, wire::Error::congestion});
  pair.run();
  ASSERT_FALSE(pair.controls.empty());
  EXPECT_EQ(pair.controls.back().sender, Sender::host);
  EXPECT_EQ(pair.controls.back().text,
            "{\"code\":\"CONGESTION\",\"type\":\"ERROR\"}");
  EXPECT_EQ(pair.host.state(), Progress::gone);
  EXPECT_EQ(count<StopMedia>(pair.host_log), 1U);
  EXPECT_EQ(count<ReleaseSeat>(pair.host_log), 1U);
  EXPECT_EQ(pair.guest.state(), Progress::leaving);
  EXPECT_EQ(count<StopMediaReception>(pair.guest_log), 1U);
  const auto sent = pair.controls.size();
  pair.to_host(LocalFailure{pair.now, wire::Error::source_format});
  pair.run();
  EXPECT_EQ(pair.controls.size(), sent);
}

TEST_F(PlayerPair, EndAGuestThatFailsBeforeConnectingSilently) {
  Pair early;
  early.to_guest(LocalFailure{early.now, wire::Error::source_format});
  early.run();
  EXPECT_EQ(early.guest.state(), Progress::gone);
  EXPECT_TRUE(early.controls.empty());
}

TEST_F(PlayerPair, ReportAGuestFailureWhilePlayingToTheHost) {
  Pair active;
  active.connect();
  active.exchange_codes(vectors().host_code);
  active.to_guest(LocalFailure{active.now, wire::Error::source_format});
  active.run();
  ASSERT_FALSE(active.controls.empty());
  EXPECT_EQ(active.controls.back().sender, Sender::guest);
  EXPECT_EQ(active.controls.back().text,
            "{\"code\":\"SOURCE_FORMAT\",\"type\":\"ERROR\"}");
  EXPECT_EQ(active.guest.state(), Progress::gone);
  EXPECT_EQ(active.host.state(), Progress::leaving);
  EXPECT_EQ(count<ReleaseSeat>(active.host_log), 1U);
}

}  // namespace
}  // namespace kakusan_test
