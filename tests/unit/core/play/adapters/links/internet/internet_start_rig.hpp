#pragma once

#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/platform/default_factory.hpp"
#include "libchiaking/platform/factory.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/play/launch_transcript.hpp"
#include "tests/unit/core/play/remote_route_network_fixture.hpp"
#include "tests/unit/core/play/remote_start_script.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace play_test::internet_starts {

using test_support::play::SignedInPlayer;

namespace platform = libchiaking::platform;
namespace play = libchiaking::play;
namespace remote_play = libchiaking::net::protocol::remote_play;
namespace script = remote_start_script;
using Route = libchiaking::progress::InternetRoute;

class HeardSetup final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (const auto* progress =
            std::get_if<remote_play::event::SetupProgress>(&event))
      heard_.push_back(*progress);
  }

  auto heard() const -> const std::vector<remote_play::event::SetupProgress>& {
    return heard_;
  }

  auto routes() const -> std::vector<remote_play::event::SetupProgress> {
    std::vector<remote_play::event::SetupProgress> routes;
    std::ranges::copy_if(
        heard_, std::back_inserter(routes),
        [](const auto& progress) { return progress.route.has_value(); });
    return routes;
  }

 private:
  std::vector<remote_play::event::SetupProgress> heard_;
};

class IgnoredEvents final : public play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent) override {}
};

class HeardRoutes final : public play::InternetStart::RouteWatcher {
 public:
  void on_route(const Route& route) override { heard_.push_back(route); }

  auto heard() const -> const std::vector<Route>& { return heard_; }

 private:
  std::vector<Route> heard_;
};

class ScriptedConsole {
 public:
  static constexpr std::string_view bearer = "SYNTHETIC_TEST_TOKEN";
  static constexpr int pump_limit = 400;

  ScriptedConsole() {
    ps_.start_status = 200;
    counts_->progress = ps_.progress;
    crypto_.allow_registration_request = true;
    crypto_.allow_stream_prepare = true;
    console_.dns_.on_stun = [progress = ps_.progress] {
      ++progress->stun_bindings;
    };
    auto& replies = console_.sockets_;
    replies.respond_to_init = true;
    replies.respond_to_cookie = true;
    replies.respond_to_registration = true;
    replies.respond_to_ctrl = true;
    replies.queue_session_id = true;
    replies.respond_to_takion_init = true;
    replies.respond_to_takion_cookie = true;
    replies.respond_to_takion_data = true;
    replies.respond_to_takion_big = true;
    replies.respond_to_senkusha_echo = true;
    replies.respond_to_rtt_echo = true;
    replies.respond_to_outbound_mtu_echo = true;
    replies.respond_to_echo_disable = true;
    replies.respond_to_inbound_mtu = true;
    replies.respond_to_client_mtu = true;
    replies.respond_to_client_mtu_final = true;
    replies.respond_to_switch_request = true;
    replies.allow_idle_poll = true;
  }

  ScriptedConsole(const ScriptedConsole&) = delete;
  auto operator=(const ScriptedConsole&) -> ScriptedConsole& = delete;

  static auto configuration() -> play::InternetLinkConfiguration {
    play::InternetLinkConfiguration config;
    config.user = SignedInPlayer::fresh_for_remote_play(
        bearer, libchiaking::AccountId::from_bytes(std::array{
                    std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4},
                    std::byte{5}, std::byte{6}, std::byte{7}, std::byte{8}}));
    config.host = "127.0.0.1";
    config.console.target = remote_play::RegistrationTarget::ps5_1;
    return config;
  }

  static void play_until_streaming(play::Link& link) {
    ASSERT_TRUE(link.start());
    for (int pump = 0; pump < pump_limit &&
                       link.state() != remote_play::StreamState::streaming;
         ++pump)
      ASSERT_TRUE(link.pump_once());
    ASSERT_EQ(link.state(), remote_play::StreamState::streaming);
  }

  auto threading() -> script::ScriptThreading& { return threading_; }
  auto clock() -> script::ScriptClock& { return clock_; }
  auto random() -> script::ScriptRandom& { return random_; }
  auto crypto() -> script::RejectCrypto& { return crypto_; }
  auto ps() -> script::ScriptHttp& { return ps_; }
  auto push() -> script::ScriptWebSocket& { return push_; }
  auto console() -> public_route_fixture::RouteNetwork& { return console_; }

 private:
  script::ScriptThreading threading_;
  script::ScriptClock clock_;
  script::ScriptRandom random_;
  script::RejectCrypto crypto_;
  script::ScriptHttp ps_;
  std::shared_ptr<script::ScriptCounts> counts_{
      std::make_shared<script::ScriptCounts>()};
  script::ScriptWebSocket push_{
      counts_,
      {script::created_body, script::members_body, script::ready_members_body,
       script::ready_custom_body, script::route_offer_body,
       script::route_result_body, script::route_accept_body,
       script::data_offer_body, script::data_result_body,
       script::data_accept_body, script::members_deleted_body}};
  public_route_fixture::RouteNetwork console_;
};

class PlayedStart {
 public:
  PlayedStart() = default;
  PlayedStart(const PlayedStart&) = delete;
  auto operator=(const PlayedStart&) -> PlayedStart& = delete;

  void play() {
    auto link = play::InternetLinks::playing(ScriptedConsole::configuration(),
                                             factory_, nullptr);
    ASSERT_TRUE(link);
    link.value()->set_event_sink(events_);
    ASSERT_NO_FATAL_FAILURE(
        ScriptedConsole::play_until_streaming(*link.value()));
    const auto heard_when_playing = events_.heard().size();
    for (int pump = 0; pump < 20; ++pump)
      ASSERT_TRUE(link.value()->pump_once());
    EXPECT_EQ(events_.heard().size(), heard_when_playing);
    EXPECT_TRUE(link.value()->stop());
    link.value().reset();
    std::uint64_t last = 0;
    for (const auto& progress : events_.heard()) {
      EXPECT_GE(progress.elapsed_us, last);
      EXPECT_TRUE(progress.text.empty());
      last = progress.elapsed_us;
    }
  }

  auto routes() const -> std::vector<remote_play::event::SetupProgress> {
    return events_.routes();
  }

 private:
  ScriptedConsole console_;
  HeardSetup events_;
  platform::DefaultPlatformFactory factory_{
      platform::PlatformProviders{.threading = &console_.threading(),
                                  .clock = &console_.clock(),
                                  .random = &console_.random(),
                                  .network = &console_.console(),
                                  .crypto = &console_.crypto(),
                                  .http = &console_.ps(),
                                  .websocket = &console_.push()}};
};

class TranscribedStart {
 public:
  static constexpr std::string_view golden = "internet_start_ps5";
  static constexpr std::string_view session_id =
      "01234567-89ab-cdef-0123-456789abcdef";

  TranscribedStart() {
    transcript_.conceal(ScriptedConsole::bearer, "bearer");
    transcript_.conceal(session_id, "session-id");
  }

  TranscribedStart(const TranscribedStart&) = delete;
  auto operator=(const TranscribedStart&) -> TranscribedStart& = delete;

  void play() {
    auto link = play::InternetLinks::playing(ScriptedConsole::configuration(),
                                             factory_, nullptr);
    ASSERT_TRUE(link);
    link.value()->set_event_sink(events_);
    ASSERT_NO_FATAL_FAILURE(
        ScriptedConsole::play_until_streaming(*link.value()));
    ASSERT_TRUE(link.value()->stop());
    link.value().reset();
  }

  auto matches_golden() const -> ::testing::AssertionResult {
    if (!play_transcript::GoldenFile{golden}.holds(transcript_))
      return ::testing::AssertionFailure()
             << golden << ": the transcript differs from the golden file";
    return ::testing::AssertionSuccess();
  }

 private:
  play_transcript::Transcript transcript_;
  ScriptedConsole console_;
  play_transcript::TranscribingHttp http_{console_.ps(), transcript_};
  play_transcript::TranscribingWebSocket websocket_{console_.push(),
                                                    transcript_};
  play_transcript::TranscribingNetwork network_{console_.console(),
                                                transcript_};
  IgnoredEvents events_;
  platform::DefaultPlatformFactory factory_{
      platform::PlatformProviders{.threading = &console_.threading(),
                                  .clock = &console_.clock(),
                                  .random = &console_.random(),
                                  .network = &network_,
                                  .crypto = &console_.crypto(),
                                  .http = &http_,
                                  .websocket = &websocket_}};
};

}  // namespace play_test::internet_starts
