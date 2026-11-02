#include "src/core/catalog/usecases/fetch_progress.hpp"

namespace libchiaking::catalog::usecases {

FetchProgress::FetchProgress(FetchBoard& board,
                             std::optional<std::uint64_t> attempt) noexcept
    : board_(attempt ? &board : nullptr), attempt_(attempt.value_or(0)) {}

void FetchProgress::began(FetchStep step,
                          std::optional<FetchActivity> activity) const {
  if (board_ != nullptr) board_->began(attempt_, step, activity);
}

void FetchProgress::doing(FetchActivity activity) const {
  if (board_ != nullptr) board_->doing(attempt_, activity);
}

void FetchProgress::asking(std::string_view host) const {
  if (board_ != nullptr) board_->asking(attempt_, host);
}

auto FetchProgress::pages() const -> PageCount {
  if (board_ == nullptr) return PageCount{};
  return PageCount{
      [board = board_, attempt = attempt_](
          std::uint32_t read, std::optional<std::uint32_t> expected) {
        board->pages(attempt, read, expected);
      }};
}

void FetchProgress::ended(StepOutcome outcome) const {
  if (board_ != nullptr) board_->ended(attempt_, outcome);
}

void FetchProgress::skipped(FetchStep step) const {
  began(step);
  ended(StepOutcome::skipped);
}

void FetchProgress::failed(const Unanswered& why) const {
  if (board_ != nullptr) board_->failed(attempt_, why);
}

void FetchProgress::finished() const {
  if (board_ != nullptr) board_->finished(attempt_);
}

}  // namespace libchiaking::catalog::usecases
