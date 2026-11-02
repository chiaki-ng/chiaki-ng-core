#pragma once

#include "libchiaking/platform/default_factory.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/play/scripted_cloud_ps.hpp"
#include "tests/support/play/signed_in_player.hpp"
#include "tests/unit/core/play/cleared_launch.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <variant>

namespace play_test::cloud_links {

using test_support::play::ScriptedCloudPs;
using test_support::play::SignedInPlayer;

namespace remote_play = libchiaking::net::protocol::remote_play;

class RejectCrypto final : public libchiaking::platform::CryptoProvider {
 public:
  using Refusal =
      libchiaking::Expected<void, libchiaking::platform::CryptoError>;

  auto sha256(std::span<const std::byte>, std::span<std::byte>)
      -> Refusal override {
    return refused();
  }
  auto hmac_sha256(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<std::byte>) -> Refusal override {
    return refused();
  }
  auto aes128_encrypt_block(std::span<const std::byte>,
                            std::span<const std::byte>, std::span<std::byte>)
      -> Refusal override {
    return refused();
  }
  auto aes128_gmac(std::span<const std::byte>, std::span<const std::byte>,
                   std::span<const std::byte>, std::span<std::byte>)
      -> Refusal override {
    return refused();
  }

 private:
  static auto refused() -> Refusal {
    return libchiaking::unexpected(
        libchiaking::platform::CryptoError::unsupported_capability);
  }
};

class UnauthorizedHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    ++calls_;
    EXPECT_NE(request.url.view().find("authorizeCheck"), std::string::npos);
    libchiaking::platform::HttpResponse response;
    response.status_code = 401;
    return response;
  }

  auto calls() const -> unsigned { return calls_; }

 private:
  unsigned calls_{};
};

class AudioHttp final : public libchiaking::platform::HttpProvider {
 public:
  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken& stop)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    if (request.url.view().find("/sessions/start") != std::string_view::npos)
      read_start(request.body.view());
    return ps.perform(request, stop);
  }

  ScriptedCloudPs ps;
  std::string selected;
  std::string preferred;
  std::string maximum;

 private:
  void read_start(std::string_view body) {
    const auto decoded = test_support::JsonTree::parse(body);
    ASSERT_TRUE(decoded);
    const auto* spec =
        test_support::JsonTree::member(*decoded, "requestGameSpecification");
    ASSERT_NE(spec, nullptr);
    selected = test_support::JsonTree::text_at(*spec, "audioChannels");
    const auto* audio =
        test_support::JsonTree::member(*spec, "audioStreamSettings");
    ASSERT_NE(audio, nullptr);
    preferred =
        test_support::JsonTree::text_at(*audio, "preferredNumberAudioChannels");
    maximum = test_support::JsonTree::text_at(*audio, "maxAudioChannels");
  }
};

class StateSink final : public libchiaking::play::LinkEventSink {
 public:
  void on_event(remote_play::StreamEvent event) override {
    if (const auto* state =
            std::get_if<remote_play::event::StateChanged>(&event)) {
      ++states;
      last_state = state->state;
    } else if (const auto* failed =
                   std::get_if<remote_play::event::Failed>(&event)) {
      ++failures;
      last_failure = failed->reason;
    } else {
      ADD_FAILURE() << "an event other than a state change or a failure";
    }
  }

  unsigned states{};
  unsigned failures{};
  remote_play::StreamState last_state{remote_play::StreamState::idle};
  remote_play::StreamFailure last_failure{
      remote_play::StreamFailure::pipeline_failed};
};

class StopOnRoute final : public libchiaking::play::LinkEventSink {
 public:
  explicit StopOnRoute(libchiaking::play::Link& session) : session_(session) {}

  void on_event(remote_play::StreamEvent event) override {
    if (const auto* state =
            std::get_if<remote_play::event::StateChanged>(&event)) {
      if (state->state == remote_play::StreamState::stopped) ++stopped_;
      if (state->state == remote_play::StreamState::establishing_route) {
        EXPECT_TRUE(session_.stop());
      }
    } else if (std::holds_alternative<remote_play::event::Failed>(event)) {
      ++failures_;
    }
  }

  auto stopped() const -> unsigned { return stopped_; }
  auto failures() const -> unsigned { return failures_; }

 private:
  libchiaking::play::Link& session_;
  unsigned stopped_{};
  unsigned failures_{};
};

class CloudLinkTest : public ::testing::Test {
 protected:
  using CloudService = libchiaking::cloud::CloudService;

  auto cleared(CloudService service = CloudService::cloud_stream)
      -> ClearedLaunch& {
    return service == CloudService::legacy_cloud_stream ? old_service_ : ps5_;
  }

  auto make_config(CloudService service = CloudService::cloud_stream)
      -> libchiaking::play::CloudLinkConfiguration {
    libchiaking::play::CloudLinkConfiguration config;
    config.user = SignedInPlayer::fresh();
    cleared(service).arm(config);
    config.time_zone = "UTC+00:00";
    return config;
  }

 private:
  ClearedLaunch ps5_{CloudService::cloud_stream};
  ClearedLaunch old_service_{CloudService::legacy_cloud_stream};
};

}  // namespace play_test::cloud_links
