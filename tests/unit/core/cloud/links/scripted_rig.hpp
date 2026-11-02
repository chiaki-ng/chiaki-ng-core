#pragma once

#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace cloud_test {

class Step {
 public:
  static auto reply(std::string_view body, std::string_view rotated_key = {},
                    std::string_view event = {}) -> Step {
    Step step;
    step.body = std::string{body};
    step.rotated_key = std::string{rotated_key};
    step.event = std::string{event};
    return step;
  }

  static auto failure(platform::HttpError error) -> Step {
    Step step;
    step.error = error;
    return step;
  }

  static auto rejection(std::uint16_t status) -> Step {
    Step step;
    step.status = status;
    return step;
  }

  std::string body;
  std::string rotated_key;
  std::string event;
  std::optional<platform::HttpError> error;
  std::uint16_t status{200};
};

class ScriptedGk final : public platform::HttpProvider {
 public:
  explicit ScriptedGk(std::string_view path) : path_(path) {}

  auto perform(const platform::HttpRequest& request, const platform::StopToken&)
      -> libchiaking::Expected<platform::HttpResponse,
                               platform::HttpError> override {
    ++calls;
    EXPECT_NE(request.url.view().find(path_), std::string::npos)
        << request.url.view();
    EXPECT_EQ(request.headers.size(), std::size_t{5});
    if (request.headers.size() > 4)
      seen_keys.emplace_back(request.headers[4].value.view());
    const auto index = static_cast<std::size_t>(calls - 1);
    EXPECT_LT(index, steps.size()) << "no step is scripted for this request";
    if (index >= steps.size())
      return libchiaking::unexpected(platform::HttpError::backend_failure);
    const auto& step = steps[index];
    if (step.error) return libchiaking::unexpected(*step.error);
    platform::HttpResponse response;
    response.status_code = step.status;
    if (!step.rotated_key.empty())
      response.headers.push_back(
          {"X-Gaikai-Session", libchiaking::SecretString{step.rotated_key}});
    if (!step.event.empty())
      response.headers.push_back(
          {"x-GAIKAI-event", libchiaking::SecretString{step.event}});
    response.body.assign(std::string_view{step.body});
    return response;
  }

  std::vector<Step> steps;
  std::vector<std::string> seen_keys;
  unsigned calls{};

 private:
  std::string path_;
};

class StageProgress final : public play::LaunchObserver {
 public:
  explicit StageProgress(
      libchiaking::net::protocol::remote_play::FailureStage expected)
      : expected_(expected) {}

  void on_progress(const play::LaunchProgress& progress,
                   libchiaking::net::protocol::remote_play::FailureStage stage,
                   std::string_view text) override {
    EXPECT_EQ(stage, expected_);
    texts.emplace_back(text);
    typed.push_back(progress);
    const auto wait = progress.wait();
    if (wait && wait->reason == libchiaking::progress::WaitReason::server_queue)
      queue_estimates.emplace_back(progress.wait_time_estimate_seconds());
  }

  std::vector<std::string> texts;
  std::vector<play::LaunchProgress> typed;
  std::vector<std::optional<std::uint32_t>> queue_estimates;

 private:
  libchiaking::net::protocol::remote_play::FailureStage expected_;
};

class ScriptedReservation {
 public:
  using Texts = std::vector<std::string>;

  static auto initial_seed() -> SessionSeed {
    SessionSeed seed;
    seed.config_key = "initial-key";
    return seed;
  }

  static auto fresh_stop() -> std::shared_ptr<test_support::StopState> {
    return std::make_shared<test_support::StopState>();
  }

  static auto repeated(std::size_t count, int milliseconds)
      -> std::vector<int> {
    return std::vector<int>(count, milliseconds);
  }
};

}  // namespace cloud_test
