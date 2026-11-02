#pragma once

#include "libchiaking/detail/diagnostics/log_queue.hpp"
#include "libchiaking/platform/default.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/home_link_configuration.hpp"
#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_link_configuration.hpp"
#include "src/core/play/adapters/links/link_driver.hpp"
#include "tests/support/catalog/idle_catalog.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/support/setup.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace api_test {

class LoopbackListener {
 public:
  LoopbackListener() {
    handle_ = ::socket(AF_INET, SOCK_STREAM, 0);
    test_support::Setup::require(handle_ >= 0, "the listener socket opens");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    test_support::Setup::require(
        ::bind(handle_, reinterpret_cast<sockaddr*>(&address),
               sizeof address) == 0,
        "the listener binds");
    test_support::Setup::require(::listen(handle_, 4) == 0,
                                 "the listener listens");
    socklen_t length = sizeof address;
    test_support::Setup::require(
        ::getsockname(handle_, reinterpret_cast<sockaddr*>(&address),
                      &length) == 0,
        "the listener has a port");
    port_ = ntohs(address.sin_port);
  }
  ~LoopbackListener() { ::close(handle_); }
  LoopbackListener(const LoopbackListener&) = delete;
  auto operator=(const LoopbackListener&) -> LoopbackListener& = delete;

  auto port() const noexcept -> std::uint16_t { return port_; }

 private:
  int handle_{-1};
  std::uint16_t port_{};
};

class RecordedSessionEvents final : public libchiaking::play::LinkDriverSink {
 public:
  using Event = libchiaking::net::protocol::remote_play::StreamEvent;

  void on_event(Event event) override {
    if (const auto* state = std::get_if<
            libchiaking::net::protocol::remote_play::event::StateChanged>(
            &event)) {
      ++state_events;
      states.push_back(*state);
    } else if (const auto* failed = std::get_if<
                   libchiaking::net::protocol::remote_play::event::Failed>(
                   &event)) {
      ++failed_events;
      failures.push_back(*failed);
    } else {
      ++other_events;
    }
    if (hook) hook(event);
  }

  void on_video_frame(libchiaking::media::VideoFrame) override {
    ++video_frames;
  }

  void reset() {
    state_events = failed_events = other_events = video_frames = 0;
    states.clear();
    failures.clear();
  }

  unsigned state_events{};
  unsigned failed_events{};
  unsigned other_events{};
  unsigned video_frames{};
  std::vector<libchiaking::net::protocol::remote_play::event::StateChanged>
      states;
  std::vector<libchiaking::net::protocol::remote_play::event::Failed> failures;
  std::function<void(const Event&)> hook;
};

class RealStream {
 public:
  static constexpr std::string_view bearer = "SYNTHETIC_BEARER_TOKEN";
  static constexpr std::string_view npsso = "SYNTHETIC_TOKEN";
  static constexpr std::string_view entitlement =
      test_support::catalog::IdleCatalog::ps5_title;

  static auto local(std::uint16_t ctrl_port = 9295)
      -> libchiaking::play::HomeLinkConfiguration {
    libchiaking::play::HomeLinkConfiguration configuration;
    configuration.host = "127.0.0.1";
    configuration.console.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1;
    configuration.ctrl_endpoint.port = ctrl_port;
    configuration.active_stream_port = 9296;
    configuration.video.width = 1280;
    configuration.video.height = 720;
    configuration.video.max_fps = 60;
    configuration.video.bitrate_kbps = 5000;
    return configuration;
  }

  static auto remote() -> libchiaking::play::InternetLinkConfiguration {
    libchiaking::play::InternetLinkConfiguration configuration;
    configuration.user =
        test_support::play::SignedInPlayer::fresh_for_remote_play(
            bearer, libchiaking::AccountId{2});
    configuration.device_uid[0] = std::byte{1};
    configuration.did[0] = std::byte{3};
    configuration.console.target =
        libchiaking::net::protocol::remote_play::RegistrationTarget::ps5_1;
    configuration.video.width = 1920;
    configuration.video.height = 1080;
    configuration.video.max_fps = 60;
    configuration.video.bitrate_kbps = 10000;
    configuration.start_timeout_us = 5'000'000;
    return configuration;
  }

  static auto cloud(libchiaking::cloud::CloudService service =
                        libchiaking::cloud::CloudService::cloud_stream)
      -> libchiaking::play::CloudLinkConfiguration {
    libchiaking::play::CloudLinkConfiguration configuration;
    configuration.user = test_support::play::SignedInPlayer::fresh();
    configuration.catalog = &test_support::catalog::IdleCatalog::shared();
    configuration.product = test_support::catalog::IdleCatalog::title(
        service == libchiaking::cloud::CloudService::legacy_cloud_stream
            ? test_support::catalog::IdleCatalog::old_service_title
            : test_support::catalog::IdleCatalog::ps5_title);
    configuration.time_zone = "UTC+00:00";
    configuration.resolution = 1080;
    configuration.bitrate_kbps = 10000;
    return configuration;
  }

  explicit RealStream(libchiaking::play::HomeLinkConfiguration configuration) {
    open(std::move(configuration), libchiaking::play::Venue::home());
  }

  explicit RealStream(
      libchiaking::play::InternetLinkConfiguration configuration) {
    open(std::move(configuration), libchiaking::play::Venue::internet());
  }

  explicit RealStream(libchiaking::play::CloudLinkConfiguration configuration) {
    const bool old_service =
        configuration.product && configuration.product->platform() ==
                                     libchiaking::catalog::Platform::ps4;
    const auto route =
        old_service ? libchiaking::play::Venue::cloud(
                          libchiaking::catalog::ServiceGeneration::old_service)
                    : libchiaking::play::Venue::cloud(
                          libchiaking::catalog::ServiceGeneration::ps5_cloud);
    open(std::move(configuration), route);
  }

  auto operator->() noexcept -> libchiaking::play::LinkDriver* {
    return &*driver;
  }
  auto operator*() noexcept -> libchiaking::play::LinkDriver& {
    return *driver;
  }

  auto drain_log() -> std::vector<libchiaking::diagnostics::LogQueue::Record> {
    std::vector<libchiaking::diagnostics::LogQueue::Record> records;
    while (auto record = platform.log().read()) records.push_back(*record);
    return records;
  }

  void pump_for(std::chrono::milliseconds duration) {
    auto& clock = platform.factory().clock();
    const auto deadline =
        clock.now_microseconds() +
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(duration)
                .count());
    while (clock.now_microseconds() < deadline) {
      test_support::Setup::require(static_cast<bool>(driver->pump_once()),
                                   "the driver pumps");
      test_support::Setup::require(
          static_cast<bool>(driver->wait(std::chrono::milliseconds{2})),
          "the driver waits");
    }
  }

  libchiaking::platform::StopSource stop;
  libchiaking::platform::DefaultPlatform platform;
  RecordedSessionEvents events;
  std::optional<libchiaking::play::LinkDriver> driver;

 private:
  void open(libchiaking::play::HomeLinkConfiguration configuration,
            std::optional<libchiaking::play::Venue> route) {
    auto checked = std::move(configuration).validated();
    test_support::Setup::require(static_cast<bool>(checked),
                                 "the home link configuration is valid");
    checked.value().stop = stop.token();
    adopt(libchiaking::play::HomeLinkConfiguration::link(
              std::move(checked.value()), platform.factory()),
          route);
  }

  void open(libchiaking::play::InternetLinkConfiguration configuration,
            std::optional<libchiaking::play::Venue> route) {
    auto checked = std::move(configuration).validated();
    test_support::Setup::require(static_cast<bool>(checked),
                                 "the internet link configuration is valid");
    checked.value().stop = stop.token();
    adopt(libchiaking::play::InternetLinks::playing(
              std::move(checked.value()), platform.factory(), nullptr),
          route);
  }

  void open(libchiaking::play::CloudLinkConfiguration configuration,
            std::optional<libchiaking::play::Venue> route) {
    auto checked = std::move(configuration).validated();
    test_support::Setup::require(static_cast<bool>(checked),
                                 "the cloud link configuration is valid");
    checked.value().stop = stop.token();
    adopt(libchiaking::play::CloudLinkConfiguration::link(
              std::move(checked.value()), platform.factory()),
          route);
  }

  void adopt(libchiaking::Expected<
                 std::unique_ptr<libchiaking::play::Link>,
                 libchiaking::net::protocol::remote_play::StreamError>
                 session,
             std::optional<libchiaking::play::Venue> route) {
    test_support::Setup::require(session && session.value(), "the link opens");
    driver.emplace(std::move(session.value()), stop,
                   platform.factory().threading(), events, platform.log(),
                   route);
  }
};

}  // namespace api_test
