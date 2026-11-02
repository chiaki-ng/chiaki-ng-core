#pragma once

#include "libchiaking/detail/diagnostics/session_journal.hpp"
#include "src/core/play/adapters/session/session_core.hpp"
#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/play/outcome_checks.hpp"
#include "tests/unit/core/play/session_fakes.hpp"
#include "tests/support/play/session_rig.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

using test_support::LiveProviders;
using test_support::play::RecordedEvents;
using test_support::play::SignedInPlayer;

class SessionEngineRig {
 public:
  using StreamError = libchiaking::net::protocol::remote_play::StreamError;
  using FailureStage = libchiaking::net::protocol::remote_play::FailureStage;
  using ScriptedSession = libchiaking::testing::ScriptedSession;

  explicit SessionEngineRig(std::optional<libchiaking::play::Venue> kind =
                                libchiaking::play::Venue::home(),
                            libchiaking::play::SessionSettings settings = {}) {
    auto owned = std::make_unique<ScriptedSession>(stop, kind);
    session = owned.get();
    const bool cloud =
        kind == libchiaking::play::Venue::cloud(
                    libchiaking::catalog::ServiceGeneration::ps5_cloud);
    runner =
        libchiaking::play::Session::Core::open(
            {.player = SignedInPlayer::fresh(),
             .console = nullptr,
             .where = cloud ? libchiaking::play::Where::cloud
                            : libchiaking::play::Where::home,
             .service = cloud ? std::optional{libchiaking::catalog::
                                                  ServiceGeneration::ps5_cloud}
                              : std::nullopt,
             .link = std::move(owned),
             .stop = stop,
             .media = media,
             .events = events,
             .clock = clock,
             .threading = LiveProviders::threading(),
             .journal = journal,
             .first_controller =
                 libchiaking::controllers::ControllerKind::dualsense,
             .settings = settings,
             .share = nullptr})
            .value();
  }

  static auto failure(StreamError error,
                      FailureStage stage = FailureStage::session_init)
      -> libchiaking::play::LinkFailure {
    return libchiaking::play::LinkFailure{stage, error};
  }

  void then(ScriptedSession::Step step) {
    session->script.push_back(std::move(step));
  }

  void streaming() {
    then([](ScriptedSession& s) {
      s.set_state(
          libchiaking::net::protocol::remote_play::StreamState::streaming);
    });
  }

  auto run() -> libchiaking::play::Outcome {
    EXPECT_TRUE(runner->run_on_this_thread());
    return runner->outcome().value();
  }

  auto logged(std::string_view text) -> std::size_t {
    while (auto entry = journal->read()) lines_.push_back(entry->line());
    std::size_t total = 0;
    for (const auto& line : lines_)
      total += line.find(text) != std::string::npos ? 1U : 0U;
    return total;
  }

  libchiaking::platform::StopSource stop;
  libchiaking::testing::ManualClock clock;
  libchiaking::testing::RecordingMedia media;
  RecordedEvents events;
  std::shared_ptr<libchiaking::diagnostics::SessionJournal> journal{
      std::make_shared<libchiaking::diagnostics::SessionJournal>(LiveProviders::threading(),
                                                                 LiveProviders::steady(), 1)};
  ScriptedSession* session{};
  std::unique_ptr<libchiaking::play::Session::Core> runner;

 private:
  std::vector<std::string> lines_;
};

}  // namespace play_test
