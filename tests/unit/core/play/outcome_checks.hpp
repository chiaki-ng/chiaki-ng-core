#pragma once

#include <libchiaking/play_types.hpp>

#include <optional>

namespace play_test {

inline auto ended_as(const libchiaking::play::Outcome& outcome,
                     libchiaking::play::Outcome::Ending ending,
                     std::optional<libchiaking::play::EndReason::Cause> cause =
                         std::nullopt) -> bool {
  return outcome.ending() == ending && outcome.cause() == cause;
}

inline auto ended_as(const libchiaking::play::Outcome& outcome,
                     libchiaking::play::ConsoleQuit::Reason reason) -> bool {
  return outcome.ending() == libchiaking::play::Outcome::Ending::console_quit &&
         outcome.quit() && outcome.quit()->reason() == reason;
}

inline auto ended_as(const libchiaking::play::Outcome& outcome,
                     libchiaking::media::MediaFailure failure) -> bool {
  return outcome.ending() == libchiaking::play::Outcome::Ending::media_failed &&
         outcome.media_fault() && outcome.media_fault()->kind == failure;
}

}  // namespace play_test
