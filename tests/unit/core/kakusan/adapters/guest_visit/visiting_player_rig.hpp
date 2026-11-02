#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/guest_visit/visiting_player.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/effect_snapshot.hpp"
#include "tests/unit/core/kakusan/pairing_vectors.hpp"
#include "tests/unit/core/kakusan/player_effects.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test::visiting {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::SeatBinding;

class VisitingPlayerTest : public ::testing::Test,
                           protected PlayerEffects<VisitingPlayer::Effects> {
 protected:
  static constexpr SeatBinding seat{3, 7};
  static constexpr std::uint64_t start_us = 1'000'000;
  static constexpr std::uint32_t home =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::home);
  static constexpr std::uint32_t south =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::south);
  static constexpr std::uint32_t east =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::east);
  static constexpr std::uint32_t north =
      static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::north);

  enum class Stage : std::uint8_t {
    waiting,
    connecting,
    channel,
    verifying,
    coded,
    entered,
    pake_b,
    confirm_b,
    admitting,
    pending,
    setup,
    source_ready,
    granted,
    ready,
    active,
  };

  struct Cleanup {
    bool receiver{};
    bool rumble{};
    bool guess{};
  };

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto vectors() -> const PairingVectors& {
    static const PairingVectors shared{crypto(), PairingVectors::Side::guest};
    return shared;
  }

  static auto setup_hash() -> const Digest256& {
    static const Digest256 shared{PairingVectors::source_ready_hash()};
    return shared;
  }

  class Rig {
   public:
    Rig() : spoke(crypto(), entropy, vectors().context, 12) {
      entropy.queue.push_back(PairingVectors::wide_of(vectors().scalar));
    }

    auto feed(VisitingPlayer::Event event) -> VisitingPlayer::Effects {
      auto effects = spoke.handle(std::move(event));
      keep(effects);
      return effects;
    }

    auto at(std::uint64_t time) -> VisitingPlayer::Effects {
      now = time;
      auto effects = spoke.on_time(time);
      keep(effects);
      return effects;
    }

    auto receive(control::Message item) -> VisitingPlayer::Effects {
      return feed(ControlReceived{now, std::move(item)});
    }

    test_support::ScriptedRandom entropy{0x60};
    VisitingPlayer spoke;
    std::uint64_t now{start_us};
    VisitingPlayer::Effects log;
    std::uint32_t allowed{libchiaking::net::kakusan::button_mask};
    std::uint64_t admitted_at{};
    std::uint64_t active_at{};

   private:
    void keep(const VisitingPlayer::Effects& effects) {
      std::ranges::transform(effects, std::back_inserter(log),
                             [](const auto& effect) {
                               return kakusan_test::Effects::snapshot(effect);
                             });
    }
  };

  static void drive(Rig& rig, Stage target) {
    if (target == Stage::waiting) return;
    auto effects = rig.feed(IceNominated{rig.now});
    EXPECT_EQ(rig.spoke.state(), Progress::connecting);
    EXPECT_EQ(shown(effects).state, Progress::connecting);
    if (target == Stage::connecting) return;
    effects = rig.feed(DtlsEstablished{rig.now, vectors().exporter.clone(),
                                       vectors().own_proof});
    EXPECT_EQ(message<control::Hello>(effects).transcript,
              vectors().context.transcript);
    if (target == Stage::channel) return;
    effects = rig.receive(control::Hello{vectors().context.transcript});
    EXPECT_EQ(rig.spoke.state(), Progress::connecting);
    EXPECT_EQ(message<control::Identity>(effects).signature,
              vectors().own_proof.signature);
    effects = rig.receive(vectors().peer_proof);
    EXPECT_EQ(the<PeerIdentified>(effects).key, vectors().peer_proof.key);
    EXPECT_EQ(rig.spoke.state(), Progress::verifying);
    if (target == Stage::verifying) return;
    effects = rig.feed(LocalCode{
        rig.now, libchiaking::net::kakusan::Code::from_value(7306).value()});
    EXPECT_EQ(code_shown(effects), vectors().guest_code.view());
    if (target == Stage::coded) return;
    effects = rig.feed(PeerCodeEntered{rig.now, vectors().host_code.clone()});
    EXPECT_TRUE(effects.empty());
    if (target == Stage::entered) return;
    effects = rig.receive(control::PakeA{vectors().pa});
    EXPECT_EQ(message<control::PakeB>(effects).point, vectors().pb);
    if (target == Stage::pake_b) return;
    effects = rig.receive(control::ConfirmA{vectors().ca});
    EXPECT_EQ(message<control::ConfirmB>(effects).mac, vectors().cb);
    if (target == Stage::confirm_b) return;
    effects = rig.receive(control::Authenticated{vectors().context.transcript});
    EXPECT_EQ(message<control::Authenticated>(effects).transcript,
              vectors().context.transcript);
    EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
    EXPECT_EQ(count<ConsumeInvitation>(effects), 1U);
    rig.admitted_at = rig.now;
    if (target == Stage::admitting) return;
    EXPECT_TRUE(rig.receive(control::AdmissionPending{}).empty());
    if (target == Stage::pending) return;
    effects = rig.receive(control::Message{vectors().setup.clone()});
    const auto* configure = first<ConfigureReceiver>(effects);
    ASSERT_NE(configure, nullptr);
    EXPECT_EQ(configure->setup.receive_key, vectors().setup.receive_key);
    EXPECT_EQ(configure->setup.video_profiles.size(), 1U);
    EXPECT_EQ(count<SendControl>(effects), 0U);
    if (target == Stage::setup) return;
    effects = rig.feed(ReceiverReady{rig.now});
    EXPECT_EQ(message<control::SourceReady>(effects).setup_hash, setup_hash());
    if (target == Stage::source_ready) return;
    effects = rig.receive(control::SeatGranted{seat, 4, 2, rig.allowed});
    EXPECT_EQ(rig.spoke.state(), Progress::getting_ready);
    EXPECT_EQ(the<PrepareController>(effects).grant.binding, seat);
    EXPECT_EQ(the<PrepareController>(effects).grant.allowed_buttons,
              rig.allowed);
    EXPECT_EQ(count<SendControl>(effects), 0U);
    EXPECT_EQ(rig.spoke.binding(), seat);
    if (target == Stage::granted) return;
    effects = rig.feed(ControllerReady{rig.now});
    EXPECT_EQ(message<control::Ready>(effects).binding, seat);
    if (target == Stage::ready) return;
    effects = rig.receive(control::Active{seat});
    EXPECT_EQ(rig.spoke.state(), Progress::playing);
    EXPECT_EQ(the<StartMediaReception>(effects).binding, seat);
    const auto* sent_input = first<SendInput>(effects);
    ASSERT_NE(sent_input, nullptr);
    EXPECT_EQ(sent_input->frame.sequence, 0U);
    EXPECT_EQ(sent_input->frame.binding, seat);
    EXPECT_EQ(sent_input->frame.elapsed_ms, 0U);
    rig.active_at = rig.now;
  }

  static void keep_alive(Rig& rig, std::uint64_t until) {
    for (auto time = rig.now + 500'000; time < until; time += 500'000) {
      const auto effects = rig.at(time);
      if (const auto* item = sent<control::Ping>(effects))
        static_cast<void>(rig.receive(
            control::Pong{std::get<control::Ping>(item->message).nonce}));
    }
  }

  static void expect_cleanup(const Rig& rig, std::size_t from,
                             Cleanup expected) {
    EXPECT_EQ(count<StopMediaReception>(rig.log, from),
              expected.receiver ? 1U : 0U);
    EXPECT_EQ(count<StopRumble>(rig.log, from), expected.rumble ? 1U : 0U);
    EXPECT_EQ(count<RecordAuthenticationFailure>(rig.log),
              expected.guess ? 1U : 0U);
    EXPECT_EQ(count<EraseSecrets>(rig.log), 1U);
    EXPECT_EQ(count<EraseSecrets>(rig.log, from), 1U);
    EXPECT_EQ(count<ConsumeInvitation>(rig.log), 1U);
    EXPECT_EQ(count<SendInput>(rig.log, from), 0U);
    EXPECT_EQ(count<ApplyRumble>(rig.log, from), 0U);
    EXPECT_EQ(count<ConfigureReceiver>(rig.log, from), 0U);
    EXPECT_EQ(count<StartMediaReception>(rig.log, from), 0U);
    std::optional<std::size_t> notice;
    for (std::size_t index = from; index < rig.log.size(); ++index) {
      const auto& effect = rig.log[index];
      if (const auto* item = std::get_if<SendControl>(&effect);
          item && item->terminal) {
        EXPECT_FALSE(notice) << "a second terminal message at " << index;
        notice = index;
      }
      if (notice) {
        EXPECT_FALSE(std::holds_alternative<StopMediaReception>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<StopRumble>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<EraseSecrets>(effect))
            << "at " << index;
      }
    }
  }

  static void expect_closed_after_grace(Rig& rig) {
    EXPECT_EQ(rig.spoke.state(), Progress::leaving);
    EXPECT_EQ(rig.spoke.next_deadline_us(), rig.now + terminal_grace_us);
    const auto now = rig.now;
    EXPECT_TRUE(rig.at(now + terminal_grace_us - 1).empty());
    const auto effects = rig.at(now + terminal_grace_us);
    EXPECT_EQ(count<CloseTransport>(effects), 1U);
    EXPECT_EQ(shown(effects).state, Progress::gone);
    EXPECT_EQ(count<CloseTransport>(rig.log), 1U);
    EXPECT_FALSE(rig.spoke.next_deadline_us());
  }

  static void expect_quiet(Rig& rig) {
    EXPECT_EQ(rig.spoke.state(), Progress::gone);
    const auto before = rig.log.size();
    static_cast<void>(rig.receive(control::Ping{}));
    static_cast<void>(rig.receive(control::Message{vectors().setup.clone()}));
    static_cast<void>(rig.receive(control::Active{seat}));
    static_cast<void>(rig.feed(Cancel{rig.now, control::Reason::user}));
    static_cast<void>(rig.feed(TransportFailed{rig.now}));
    static_cast<void>(rig.feed(MalformedData{rig.now, Error::protocol}));
    static_cast<void>(rig.feed(FeedbackReceived{
        rig.now, libchiaking::net::kakusan::FeedbackFrame{seat, 99, 0, 5, 5}}));
    static_cast<void>(rig.feed(
        ControllerSampled{rig.now, libchiaking::net::kakusan::InputState{}}));
    static_cast<void>(rig.feed(ReceiverReady{rig.now}));
    static_cast<void>(rig.feed(ControllerReady{rig.now}));
    static_cast<void>(rig.feed(IdrNeeded{rig.now}));
    static_cast<void>(rig.at(rig.now + 100'000'000));
    EXPECT_EQ(rig.log.size(), before);
  }

  static void answer_ping(Rig& rig, const VisitingPlayer::Effects& effects) {
    if (const auto* ping = sent<control::Ping>(effects))
      static_cast<void>(rig.receive(
          control::Pong{std::get<control::Ping>(ping->message).nonce}));
  }

  static auto feedback(std::uint32_t sequence, std::uint16_t low,
                       SeatBinding binding = seat)
      -> libchiaking::net::kakusan::FeedbackFrame {
    libchiaking::net::kakusan::FeedbackFrame frame;
    frame.binding = binding;
    frame.sequence = sequence;
    frame.low_frequency = low;
    return frame;
  }

  static auto pressed(std::uint32_t buttons)
      -> libchiaking::net::kakusan::InputState {
    libchiaking::net::kakusan::InputState state;
    state.buttons = buttons;
    return state;
  }

  static auto all_messages() -> std::vector<control::Message> {
    control::Ping ping;
    ping.nonce[0] = std::byte{9};
    std::vector<control::Message> messages;
    messages.emplace_back(control::Hello{vectors().context.transcript});
    messages.emplace_back(vectors().peer_proof);
    messages.emplace_back(control::Authenticated{vectors().context.transcript});
    messages.emplace_back(control::AdmissionPending{});
    messages.emplace_back(vectors().setup.clone());
    messages.emplace_back(control::SourceReady{setup_hash()});
    messages.emplace_back(control::SourceIdr{});
    messages.emplace_back(control::SeatGranted{
        seat, 4, 2, libchiaking::net::kakusan::button_mask});
    messages.emplace_back(control::Ready{seat});
    messages.emplace_back(control::Active{seat});
    messages.emplace_back(control::SeatRevoked{seat, control::Reason::user});
    messages.emplace_back(control::Leave{control::Reason::user});
    messages.emplace_back(control::End{control::Reason::user});
    messages.emplace_back(ping);
    messages.emplace_back(control::Pong{ping.nonce});
    return messages;
  }

  static auto permitted(Stage stage, const control::Message& item) -> bool {
    if (is<control::Ping>(item) || is<control::Pong>(item) ||
        is<control::End>(item))
      return stage >= Stage::admitting;
    if (is<control::SeatRevoked>(item)) return stage >= Stage::granted;
    switch (stage) {
      case Stage::channel:
        return is<control::Hello>(item);
      case Stage::confirm_b:
        return is<control::Authenticated>(item);
      case Stage::admitting:
        return is<control::AdmissionPending>(item);
      case Stage::pending:
        return is<control::SourceSetup>(item);
      case Stage::source_ready:
        return is<control::SeatGranted>(item);
      case Stage::ready:
        return is<control::Active>(item);
      default:
        return false;
    }
  }
};

}  // namespace kakusan_test::visiting
