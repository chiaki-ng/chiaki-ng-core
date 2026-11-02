#pragma once

#include "tests/support/api/fakes.hpp"

#include "libchiaking/detail/media/media_sink.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "libchiaking/testing/manual_clock.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "src/core/play/adapters/session/command_route.hpp"
#include "src/core/play/adapters/session/locked_session_lifecycle.hpp"
#include "tests/unit/core/play/recording_media.hpp"
#include "tests/unit/core/play/scripted_session.hpp"
#include "tests/support/live_providers.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace api_test {

using test_support::FakeLog;

namespace play = libchiaking::play;
using libchiaking::Expected;
using libchiaking::unexpected;
using libchiaking::net::protocol::remote_play::StreamError;
using libchiaking::net::protocol::remote_play::StreamState;

using libchiaking::testing::ManualClock;
using libchiaking::testing::RecordingMedia;
using libchiaking::testing::ScriptedSession;

class IgnoredDriverEvents final : public play::LinkDriverSink {
 public:
  void on_event(libchiaking::net::protocol::remote_play::StreamEvent) override {
  }
  void on_video_frame(libchiaking::media::VideoFrame) override {}
};

class MemberRig {
 public:
  explicit MemberRig(std::optional<libchiaking::play::Venue> kind =
                         libchiaking::play::Venue::home())
      : session(new ScriptedSession{stop, kind}),
        driver(std::unique_ptr<play::Link>{session}, stop, test_support::LiveProviders::threading(),
               sink, log, kind),
        route(driver, lifecycle, log, kind) {}

  auto logged(std::string_view text) const -> std::size_t {
    std::size_t total = 0;
    for (const auto& line : log.messages)
      total += line.find(text) != std::string::npos ? 1U : 0U;
    return total;
  }

  libchiaking::platform::StopSource stop;
  FakeLog log;
  IgnoredDriverEvents sink;
  ManualClock clock;
  ScriptedSession* session;
  play::LockedSessionLifecycle lifecycle{
      test_support::LiveProviders::threading()};
  play::LinkDriver driver;
  play::CommandRoute route;
};

inline auto bytes_of(std::initializer_list<int> values)
    -> std::vector<std::byte> {
  std::vector<std::byte> bytes;
  for (const auto value : values)
    bytes.push_back(static_cast<std::byte>(value));
  return bytes;
}

inline auto stream_info(std::size_t profiles = 1)
    -> libchiaking::net::protocol::remote_play::event::StreamInfoReady {
  libchiaking::net::protocol::remote_play::event::StreamInfoReady info;
  for (std::size_t index = 0; index < profiles; ++index)
    info.profiles.push_back({1280, 720, bytes_of({1, 2, 3})});
  info.audio.channels = 2;
  info.audio.rate = 48000;
  return info;
}

inline auto video_media(std::uint16_t frame, std::uint8_t profile = 0)
    -> libchiaking::net::protocol::remote_play::event::EncodedMedia {
  libchiaking::net::protocol::remote_play::event::EncodedMedia media{
      libchiaking::net::protocol::remote_play::EncodedMediaKind::video,
      bytes_of({4, 5, 6})};
  media.frame_index = frame;
  media.adaptive_stream_index = profile;
  return media;
}

}  // namespace api_test
