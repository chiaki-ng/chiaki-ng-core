#pragma once

#include "src/core/net/kakusan/protocol_error.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/kakusan/adapters/controller_map.hpp"
#include "src/core/kakusan/adapters/guest_visit/guest_visit_core.hpp"
#include "src/core/kakusan/adapters/shared_session/shared_session_core.hpp"
#include "src/core/net/kakusan/control_message.hpp"
#include "src/core/net/kakusan/digest.hpp"
#include "src/core/net/kakusan/result.hpp"
#include "src/core/utils/memory/constant_time.hpp"
#include "src/core/utils/timing/deadline.hpp"
#include "tests/support/crypto/software_crypto.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/support/kakusan/loopback_transport.hpp"
#include "tests/support/kakusan/messages.hpp"
#include "tests/support/play/source_stream.hpp"
#include "tests/support/process_threading.hpp"
#include "tests/support/random.hpp"
#include "tests/unit/core/kakusan/forwarded_media.hpp"
#include "tests/unit/core/kakusan/identities.hpp"
#include "tests/unit/core/kakusan/source_description.hpp"
#include "tests/unit/core/kakusan/source_vectors.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace kakusan_test {

using test_support::play::SourceStream;

class EndToEndTest : public ::testing::Test {
 protected:
  using GuestVisit = libchiaking::kakusan::GuestVisit;
  using SharedSession = libchiaking::kakusan::SharedSession;
  using Progress = libchiaking::kakusan::Progress;
  using Clock = libchiaking::kakusan::Clock;

  static constexpr std::uint64_t start_wall = 1'791'115'200;

  static auto crypto() -> test_support::SoftwareCrypto& {
    static test_support::SoftwareCrypto shared;
    return shared;
  }

  static auto identity(std::uint8_t tag)
      -> libchiaking::kakusan::DeviceIdentity {
    return Identities::tagged(crypto(), tag);
  }

  static auto text_of(const libchiaking::net::kakusan::Code& code)
      -> std::string {
    return std::string{code.begin(), code.end()};
  }

  class Player {
   public:
    Player(std::shared_ptr<test_support::kakusan::LoopbackExchange> exchange,
           std::uint8_t tag, const std::uint64_t& now)
        : entropy(0x5000 + tag),
          factory(std::move(exchange)),
          clock(now),
          session(GuestVisit::Core::create(
                      GuestVisit::Core::Dependencies{platform, entropy, factory,
                                                     media},
                      identity(tag))
                      .value()) {}

    void drain(std::uint64_t now) {
      while (auto output = session->take_output()) {
        if (auto* ready = std::get_if<GuestVisit::Core::ReplyReady>(&*output))
          reply = std::string{ready->block.view()};
        else if (const auto* shown =
                     std::get_if<GuestVisit::Core::CodeReady>(&*output))
          code = text_of(shown->code);
        else if (const auto* state =
                     std::get_if<GuestVisit::Core::State>(&*output))
          states.push_back(*state);
        else if (auto* stream =
                     std::get_if<GuestVisit::Core::StreamReady>(&*output)) {
          EXPECT_FALSE(media.descriptions.empty());
          if (!media.descriptions.empty()) info = media.descriptions.back();
          fps = stream->video_fps;
        } else if (const auto* requested =
                       std::get_if<GuestVisit::Core::ControllerRequested>(
                           &*output)) {
          controller = *requested;
          EXPECT_TRUE(session->controller_ready(now));
        } else if (const auto* rumble =
                       std::get_if<GuestVisit::Core::Rumble>(&*output))
          rumbles.push_back(*rumble);
        else if (std::holds_alternative<GuestVisit::Core::StreamStopped>(
                     *output))
          ++stopped;
        else if (auto* noted =
                     std::get_if<GuestVisit::Core::Diagnostic>(&*output))
          diagnostics.push_back(std::move(noted->text));
      }
    }

    test_support::SeededRandom entropy;
    test_support::kakusan::LoopbackFactory factory;
    ForwardedMedia media;
    TrackedClock clock;
    libchiaking::platform::DefaultPlatformFactory platform{
        libchiaking::platform::PlatformProviders{.clock = &clock,
                                                 .crypto = &crypto()}};
    std::unique_ptr<GuestVisit::Core> session;
    libchiaking::net::kakusan::InvitationId id{};
    std::string reply;
    std::string code;
    std::optional<libchiaking::media::StreamDescription> info;
    std::uint32_t fps{};
    std::optional<GuestVisit::Core::ControllerRequested> controller;
    std::vector<GuestVisit::Core::State> states;
    std::vector<GuestVisit::Core::Rumble> rumbles;
    std::size_t stopped{};
    std::vector<std::string> diagnostics;
  };

  class World {
   public:
    struct Input : libchiaking::controllers::ControllerState {
      std::uint8_t controller_id{};
    };

    auto clock() const -> Clock {
      return Clock{now, start_wall + now / 1'000'000};
    }

    void start_source() { start_source(SourceStream::description()); }

    void start_source(const libchiaking::media::StreamDescription& info) {
      host->source_keys(takion_version, SourceStream::receive_keys());
      EXPECT_TRUE(host->source_started(info, SourceStream::video_fps, now));
    }

    void drain_host() {
      while (auto output = host->take_output()) {
        if (auto* ready =
                std::get_if<SharedSession::Core::InvitationReady>(&*output))
          invitations.push_back(std::move(*ready));
        else if (const auto* reply =
                     std::get_if<SharedSession::Core::GuestIdentified>(
                         &*output))
          bound.push_back(*reply);
        else if (const auto* code =
                     std::get_if<SharedSession::Core::CodeReady>(&*output))
          codes.push_back(
              SharedSession::Core::CodeReady{code->guest, code->code.clone()});
        else if (const auto* state =
                     std::get_if<SharedSession::Core::GuestState>(&*output))
          states.push_back(*state);
        else if (const auto* join =
                     std::get_if<SharedSession::Core::JoinRequested>(
                         &*output)) {
          joins.push_back(*join);
          pads[join->guest] = next_pad;
          EXPECT_TRUE(host->source_joined(
              join->seat,
              libchiaking::controllers::SeatNumber::create(next_pad++).value(),
              now));
        } else if (const auto* removal =
                       std::get_if<SharedSession::Core::RemovalRequested>(
                           &*output))
          removals.push_back(*removal);
        else if (const auto* input =
                     std::get_if<SharedSession::Core::ControllerInput>(
                         &*output))
          inputs.push_back(Input{input->state, pads.at(input->guest)});
        else if (std::holds_alternative<SharedSession::Core::IdrRequested>(
                     *output))
          ++idr_requests;
        else if (const auto* changed =
                     std::get_if<SharedSession::Core::SeatsChanged>(&*output))
          seats.push_back(changed->seats);
        else if (auto* noted =
                     std::get_if<SharedSession::Core::Diagnostic>(&*output))
          diagnostics.push_back(std::move(noted->text));
      }
    }

    void step(std::uint64_t advance_us = 1'000) {
      now += advance_us;
      host->pump(clock());
      drain_host();
      for (auto& player : players) {
        player->session->pump(clock());
        player->drain(now);
      }
      drain_host();
    }

    template <class Done>
    void until(Done done, std::size_t limit = 20'000) {
      for (std::size_t index = 0; index < limit && !done(); ++index) step();
      EXPECT_TRUE(done());
    }

    auto host_state(const libchiaking::net::kakusan::InvitationId& guest) const
        -> Progress {
      auto found = Progress::waiting;
      for (const auto& state : states) {
        if (state.guest == guest) found = state.state;
      }
      return found;
    }

    auto host_code(const libchiaking::net::kakusan::InvitationId& guest) const
        -> std::string {
      for (const auto& code : codes) {
        if (code.guest == guest) return text_of(code.code);
      }
      return {};
    }

    auto join(std::uint8_t tag) -> Player& {
      const auto issued = invitations.size();
      const auto guest = host->invite(clock());
      EXPECT_TRUE(guest);
      until([&] { return invitations.size() > issued; });
      EXPECT_EQ(invitations.back().guest, guest.value());

      players.push_back(std::make_unique<Player>(exchange, tag, now));
      auto& player = *players.back();
      player.id = guest.value();
      const auto preview = GuestVisit::Core::preview(
          crypto(), invitations.back().block.view(), clock().wall_seconds);
      EXPECT_TRUE(preview);
      const auto summary = player.session->accept_invitation(
          invitations.back().block.view(), clock());
      EXPECT_TRUE(summary);
      if (summary && preview) {
        EXPECT_EQ(summary.value().host_hint, preview.value().host_hint);
      }
      until([&] { return !player.reply.empty(); });

      const auto imported = host->import_reply(player.reply, clock());
      EXPECT_TRUE(imported);
      if (imported) {
        EXPECT_EQ(imported.value(), guest.value());
      }
      EXPECT_TRUE(host->approve(guest.value(), now));
      until([&] {
        return !player.code.empty() && !host_code(player.id).empty();
      });

      EXPECT_TRUE(host->enter_code(player.id, player.code, now));
      EXPECT_TRUE(player.session->enter_code(host_code(player.id), now));
      until([&] {
        return player.session->state() == Progress::playing &&
               host_state(player.id) == Progress::playing;
      });
      for (std::size_t index = 0; index < 20; ++index) step();
      return player;
    }

    void forward_source() {
      for (std::size_t index = 0; index < SourceStream::vectors.size();
           ++index) {
        const auto& vector = SourceStream::vectors[index];
        const auto bytes = SourceDescriptions::relayout(
            crypto(), SourceStream::receive_keys(), takion_version,
            vector.media, vector.key_position, SourceStream::datagram(index));
        const auto described = SourceDescriptions::describe(
            takion_version, vector.media, vector.key_position, bytes);
        EXPECT_GT(described.sealed_size, 0U);
        host->source_datagram(described);
        step();
      }
      for (std::size_t index = 0; index < 100; ++index) step();
    }

    std::shared_ptr<test_support::kakusan::LoopbackExchange> exchange{
        std::make_shared<test_support::kakusan::LoopbackExchange>(crypto())};
    test_support::SeededRandom host_entropy{0x1001};
    test_support::kakusan::LoopbackFactory host_factory{exchange};
    std::unique_ptr<SharedSession::Core> host{
        SharedSession::Core::create(
            SharedSession::Core::Dependencies{
                crypto(), host_entropy, host_factory,
                test_support::process_threading()},
            identity(0x10),
            *libchiaking::kakusan::SharingRules::create(
                4, 1, libchiaking::net::kakusan::button_mask))
            .value()};
    std::vector<std::unique_ptr<Player>> players;
    std::uint64_t now{2'000'000};
    std::uint32_t takion_version{SourceStream::takion_version};

    std::vector<SharedSession::Core::InvitationReady> invitations;
    std::vector<SharedSession::Core::GuestIdentified> bound;
    std::vector<SharedSession::Core::CodeReady> codes;
    std::vector<SharedSession::Core::GuestState> states;
    std::vector<SharedSession::Core::JoinRequested> joins;
    std::vector<SharedSession::Core::RemovalRequested> removals;
    std::vector<Input> inputs;
    std::map<libchiaking::net::kakusan::InvitationId, std::uint8_t> pads;
    std::vector<libchiaking::kakusan::SeatSummary> seats;
    std::size_t idr_requests{};
    std::vector<std::string> diagnostics;
    std::uint8_t next_pad{1};
  };

  static void expect_decoded(const Player& player) {
    const auto video = player.media.video;
    const auto audio = player.media.audio;
    ASSERT_EQ(video.size(), SourceStream::video_frames.size());
    ASSERT_EQ(audio.size(), SourceStream::audio_frames.size());
    for (std::size_t index = 0; index < video.size(); ++index)
      EXPECT_EQ(video[index],
                test_support::Bytes::unhex(SourceStream::video_frames[index]));
    for (std::size_t index = 0; index < audio.size(); ++index)
      EXPECT_EQ(audio[index],
                test_support::Bytes::unhex(SourceStream::audio_frames[index]));
  }

  static auto pressed() -> libchiaking::net::kakusan::InputState {
    libchiaking::net::kakusan::InputState state;
    state.buttons =
        static_cast<std::uint32_t>(libchiaking::net::kakusan::Button::south);
    state.left_x = 12000;
    state.right_y = -32767;
    state.left_trigger = 65535;
    return state;
  }
};

}  // namespace kakusan_test
