#include "libchiaking/detail/catalog/fetch_state.hpp"

#include <utility>

namespace libchiaking::catalog {

FetchState::FetchState(std::uint64_t attempt, FetchStep step,
                       std::optional<FetchActivity> activity,
                       std::uint32_t pages_read,
                       std::optional<std::uint32_t> pages_expected,
                       std::optional<std::string> time_server,
                       std::vector<PassedStep> passed,
                       std::optional<GamesFailure> failure)
    : attempt_(attempt),
      step_(step),
      activity_(activity),
      pages_read_(pages_read),
      pages_expected_(pages_expected),
      time_server_(std::move(time_server)),
      passed_(std::move(passed)),
      failure_(std::move(failure)) {}

auto FetchState::status() const -> progress::Status {
  progress::Status status;
  if (pages_expected_)
    status.position = progress::Position{pages_read_, *pages_expected_};
  return status;
}

auto FetchState::is_running() const noexcept -> bool {
  switch (step_) {
    case FetchStep::idle:
    case FetchStep::ready:
    case FetchStep::unanswered:
    case FetchStep::cancelled:
      return false;
    case FetchStep::checking_sign_in:
    case FetchStep::cloud_list:
    case FetchStep::owned_library:
    case FetchStep::premium_category:
    case FetchStep::for_sale_category:
    case FetchStep::streaming_access:
    case FetchStep::matching:
    case FetchStep::trusted_time:
    case FetchStep::exclusions:
      break;
  }
  return true;
}

}  // namespace libchiaking::catalog
