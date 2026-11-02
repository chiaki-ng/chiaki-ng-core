#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "src/core/kakusan/adapters/shared_session/invited_player.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/peer_link.hpp"
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

namespace kakusan_test::invited {

using namespace libchiaking::kakusan;
using namespace libchiaking::net::kakusan;
namespace control = libchiaking::net::kakusan::control;
using libchiaking::net::kakusan::SeatBinding;

class InvitedPlayerTest : public ::testing::Test,
                          protected PlayerEffects<InvitedPlayer::Effects> {
 protected:
  static constexpr SeatBinding seat{3, 7};
  static constexpr std::uint64_t start_us = 1'000'000;

  enum class Stage : std::uint8_t {
    waiting,
    connecting,
    nominated,
    channel,
    verifying,
    coded,
    pake_a,
    confirm_a,
    authenticated_sent,
    admitting,
    reserved,
    setup_sent,
    setup_acked,
    ready_wait,
    grant_acked,
    active,
    media,
  };

  struct Cleanup {
    bool media{};
    bool neutral{};
    bool removal{};
    bool release{};
    bool guess{};
  };

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto vectors() -> const PairingVectors& {
    static const PairingVectors shared{crypto(), PairingVectors::Side::host};
    return shared;
  }

  static auto setup_hash() -> const Digest256& {
    static const Digest256 shared = [] {
      const auto hash =
          control::MessageCodec::setup_hash(crypto(), vectors().setup);
      EXPECT_TRUE(hash);
      return hash ? hash.value() : Digest256{};
    }();
    return shared;
  }

  class Rig {
   public:
    Rig() : spoke(crypto(), entropy) {
      entropy.queue.push_back(PairingVectors::wide_of(vectors().scalar));
    }

    auto feed(InvitedPlayer::Event event) -> InvitedPlayer::Effects {
      auto effects = spoke.handle(std::move(event));
      keep(effects);
      return effects;
    }

    auto at(std::uint64_t time) -> InvitedPlayer::Effects {
      now = time;
      auto effects = spoke.on_time(time);
      keep(effects);
      return effects;
    }

    auto receive(control::Message item) -> InvitedPlayer::Effects {
      return feed(ControlReceived{now, std::move(item)});
    }

    auto ack(AckToken token) -> InvitedPlayer::Effects {
      return feed(ControlAcked{now, token});
    }

    test_support::ScriptedRandom entropy{0x40};
    InvitedPlayer spoke;
    std::uint64_t now{start_us};
    InvitedPlayer::Effects log;
    AckToken hello_token{};
    AckToken setup_token{};
    AckToken grant_token{};
    AckToken active_token{};
    std::uint64_t admitted_at{};
    std::uint64_t active_at{};

   private:
    void keep(const InvitedPlayer::Effects& effects) {
      std::ranges::transform(effects, std::back_inserter(log),
                             [](const auto& effect) {
                               return kakusan_test::Effects::snapshot(effect);
                             });
    }
  };

  static void drive(Rig& rig, Stage target, bool approve = true) {
    if (target == Stage::waiting) return;
    auto effects = rig.feed(ReplyImported{rig.now, vectors().context});
    EXPECT_EQ(rig.spoke.state(), Progress::connecting);
    EXPECT_EQ(shown(effects).state, Progress::connecting);
    if (target == Stage::connecting) return;
    effects = rig.feed(IceNominated{rig.now});
    EXPECT_TRUE(effects.empty());
    if (target == Stage::nominated) return;
    effects = rig.feed(DtlsEstablished{rig.now, vectors().exporter.clone(),
                                       vectors().own_proof});
    EXPECT_EQ(message<control::Hello>(effects).transcript,
              vectors().context.transcript);
    const auto* hello = sent<control::Hello>(effects);
    ASSERT_NE(hello, nullptr);
    rig.hello_token = hello->token;
    if (approve) {
      EXPECT_TRUE(rig.feed(AdmissionApproved{rig.now}).empty());
    }
    if (target == Stage::channel) return;
    effects = rig.receive(control::Hello{vectors().context.transcript});
    EXPECT_EQ(rig.spoke.state(), Progress::connecting);
    EXPECT_EQ(message<control::Identity>(effects).signature,
              vectors().own_proof.signature);
    effects = rig.receive(vectors().peer_proof);
    EXPECT_EQ(the<PeerIdentified>(effects).key, vectors().peer_proof.key);
    EXPECT_EQ(rig.spoke.state(), Progress::verifying);
    EXPECT_EQ(shown(effects).state, Progress::verifying);
    if (target == Stage::verifying) return;
    effects = rig.feed(LocalCode{
        rig.now, libchiaking::net::kakusan::Code::from_value(4821).value()});
    EXPECT_EQ(code_shown(effects), vectors().host_code.view());
    EXPECT_EQ(sent<control::PakeA>(effects), nullptr);
    if (target == Stage::coded) return;
    effects = rig.feed(PeerCodeEntered{rig.now, vectors().guest_code.clone()});
    EXPECT_EQ(message<control::PakeA>(effects).point, vectors().pa);
    if (target == Stage::pake_a) return;
    effects = rig.receive(control::PakeB{vectors().pb});
    EXPECT_EQ(message<control::ConfirmA>(effects).mac, vectors().ca);
    if (target == Stage::confirm_a) return;
    effects = rig.receive(control::ConfirmB{vectors().cb});
    EXPECT_EQ(message<control::Authenticated>(effects).transcript,
              vectors().context.transcript);
    EXPECT_EQ(rig.spoke.state(), Progress::verifying);
    if (target == Stage::authenticated_sent) return;
    effects = rig.receive(control::Authenticated{vectors().context.transcript});
    EXPECT_EQ(rig.spoke.state(), Progress::waiting_for_seat);
    EXPECT_EQ(count<ConsumeInvitation>(effects), 1U);
    EXPECT_EQ(count<ReserveSeat>(effects), approve ? 1U : 0U);
    rig.admitted_at = rig.now;
    if (target == Stage::admitting) return;
    ASSERT_TRUE(approve);
    effects = rig.feed(SeatReserved{rig.now, seat});
    EXPECT_NE(sent<control::AdmissionPending>(effects), nullptr);
    EXPECT_EQ(the<RequestSourceJoin>(effects).binding, seat);
    if (target == Stage::reserved) return;
    effects = rig.feed(SourceJoined{rig.now, vectors().setup.clone(), 4, 2,
                                    libchiaking::net::kakusan::button_mask});
    const auto* setup = sent<control::SourceSetup>(effects);
    ASSERT_NE(setup, nullptr);
    const auto sent_hash = control::MessageCodec::setup_hash(
        crypto(), std::get<control::SourceSetup>(setup->message));
    ASSERT_TRUE(sent_hash);
    EXPECT_EQ(sent_hash.value(), setup_hash());
    rig.setup_token = setup->token;
    if (target == Stage::setup_sent) return;
    EXPECT_TRUE(rig.ack(rig.setup_token).empty());
    if (target == Stage::setup_acked) return;
    effects = rig.receive(control::SourceReady{setup_hash()});
    EXPECT_EQ(the<GrantSeat>(effects).binding, seat);
    const auto& grant = message<control::SeatGranted>(effects);
    EXPECT_EQ(grant.binding, seat);
    EXPECT_EQ(grant.capacity, 4U);
    EXPECT_EQ(grant.local_seats, 2U);
    EXPECT_EQ(grant.allowed_buttons, libchiaking::net::kakusan::button_mask);
    const auto* granted = sent<control::SeatGranted>(effects);
    ASSERT_NE(granted, nullptr);
    rig.grant_token = granted->token;
    EXPECT_EQ(rig.spoke.state(), Progress::getting_ready);
    if (target == Stage::ready_wait) return;
    EXPECT_TRUE(rig.ack(rig.grant_token).empty());
    if (target == Stage::grant_acked) return;
    effects = rig.receive(control::Ready{seat});
    EXPECT_EQ(message<control::Active>(effects).binding, seat);
    EXPECT_EQ(count<StartMedia>(effects), 0U);
    const auto* active = sent<control::Active>(effects);
    ASSERT_NE(active, nullptr);
    rig.active_token = active->token;
    rig.active_at = rig.now;
    EXPECT_EQ(rig.spoke.state(), Progress::playing);
    if (target == Stage::active) return;
    effects = rig.ack(rig.active_token);
    EXPECT_EQ(the<StartMedia>(effects).binding, seat);
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
    EXPECT_EQ(count<StopMedia>(rig.log, from), expected.media ? 1U : 0U);
    EXPECT_EQ(count<InjectNeutral>(rig.log, from), expected.neutral ? 1U : 0U);
    EXPECT_EQ(count<RequestSourceRemoval>(rig.log, from),
              expected.removal ? 1U : 0U);
    EXPECT_EQ(count<ReleaseSeat>(rig.log, from), expected.release ? 1U : 0U);
    EXPECT_EQ(count<RecordAuthenticationFailure>(rig.log),
              expected.guess ? 1U : 0U);
    EXPECT_EQ(count<EraseSecrets>(rig.log), 1U);
    EXPECT_EQ(count<ConsumeInvitation>(rig.log), 1U);
    EXPECT_EQ(count<StartMedia>(rig.log, from), 0U);
    EXPECT_EQ(count<ApplyInput>(rig.log, from), 0U);
    std::optional<std::size_t> notice;
    for (std::size_t index = from; index < rig.log.size(); ++index) {
      const auto& effect = rig.log[index];
      if (const auto* item = std::get_if<SendControl>(&effect);
          item && item->terminal) {
        EXPECT_FALSE(notice) << "a second terminal message at " << index;
        notice = index;
      }
      if (notice) {
        EXPECT_FALSE(std::holds_alternative<StopMedia>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<InjectNeutral>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<RequestSourceRemoval>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<ReleaseSeat>(effect))
            << "at " << index;
        EXPECT_FALSE(std::holds_alternative<EraseSecrets>(effect))
            << "at " << index;
      }
    }
    EXPECT_EQ(count<EraseSecrets>(rig.log, from), 1U);
    if (expected.media) {
      EXPECT_EQ(the<StopMedia>(rig.log, from).binding, seat);
    }
    if (expected.neutral) {
      EXPECT_EQ(the<InjectNeutral>(rig.log, from).binding, seat);
    }
    if (expected.removal) {
      EXPECT_EQ(the<RequestSourceRemoval>(rig.log, from).binding, seat);
    }
    if (expected.release) {
      EXPECT_EQ(the<ReleaseSeat>(rig.log, from).binding, seat);
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
    EXPECT_EQ(rig.spoke.state(), Progress::gone);
    EXPECT_EQ(count<CloseTransport>(rig.log), 1U);
    EXPECT_FALSE(rig.spoke.next_deadline_us());
  }

  static void expect_quiet(Rig& rig) {
    EXPECT_EQ(rig.spoke.state(), Progress::gone);
    const auto before = rig.log.size();
    static_cast<void>(rig.receive(control::Ping{}));
    static_cast<void>(rig.receive(control::Message{vectors().setup.clone()}));
    static_cast<void>(rig.feed(Cancel{rig.now, control::Reason::user}));
    static_cast<void>(rig.feed(CapacityRevoked{rig.now}));
    static_cast<void>(rig.feed(SourceEnded{rig.now}));
    static_cast<void>(rig.feed(TransportFailed{rig.now}));
    static_cast<void>(rig.feed(MalformedData{rig.now, Error::protocol}));
    static_cast<void>(rig.feed(InputReceived{
        rig.now, libchiaking::net::kakusan::InputFrame{seat, 99, 0, {}}}));
    static_cast<void>(rig.feed(RumbleSampled{rig.now, 1, 1}));
    static_cast<void>(rig.feed(AdmissionApproved{rig.now}));
    static_cast<void>(rig.at(rig.now + 100'000'000));
    EXPECT_EQ(rig.log.size(), before);
  }

  static auto input(std::uint32_t sequence, std::uint32_t buttons,
                    SeatBinding binding = seat)
      -> libchiaking::net::kakusan::InputFrame {
    libchiaking::net::kakusan::InputFrame frame;
    frame.binding = binding;
    frame.sequence = sequence;
    frame.state.buttons = buttons;
    frame.state.left_x = 100;
    return frame;
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
        is<control::Leave>(item))
      return stage >= Stage::admitting;
    switch (stage) {
      case Stage::channel:
        return is<control::Hello>(item);
      case Stage::authenticated_sent:
        return is<control::Authenticated>(item);
      case Stage::setup_sent:
      case Stage::setup_acked:
        return is<control::SourceReady>(item);
      case Stage::ready_wait:
      case Stage::grant_acked:
        return is<control::Ready>(item);
      case Stage::active:
      case Stage::media:
        return is<control::SourceIdr>(item);
      default:
        return false;
    }
  }
};

}  // namespace kakusan_test::invited
