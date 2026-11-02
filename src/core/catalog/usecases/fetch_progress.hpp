/// @file
/// @brief Declares usecases::FetchProgress, which feeds the steps, activities
/// and pages of one fetch attempt to the account's FetchBoard.
#pragma once

#include "libchiaking/detail/catalog/fetch_state.hpp"
#include "src/core/catalog/adapters/fetch_board.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/unanswered.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace libchiaking::catalog::usecases {

/// @brief Reports one fetch attempt to its board, or to nobody.
///
/// Holds a pointer to the board, which must outlive it, and the attempt every
/// event is tagged with. Made without a board it reports nothing, for the
/// reads that run outside a full fetch. Used on the thread running the
/// fetch.
class FetchProgress {
 public:
  /// @brief Reports to nobody.
  FetchProgress() noexcept = default;
  /// @brief Reports an attempt to a board.
  ///
  /// @param board The account's board; must outlive this object.
  /// @param attempt The attempt, or nothing when the board refused to start
  ///     one, in which case nothing is reported.
  FetchProgress(FetchBoard& board,
                std::optional<std::uint64_t> attempt) noexcept;

  /// @brief Reports that the next step begins.
  ///
  /// @param step The step.
  /// @param activity What it starts doing, when known.
  void began(FetchStep step,
             std::optional<FetchActivity> activity = std::nullopt) const;
  /// @brief Reports what the current step does now.
  ///
  /// @param activity The activity.
  void doing(FetchActivity activity) const;
  /// @brief Reports the time server the trusted-time step is asking.
  ///
  /// @param host The server's host name.
  void asking(std::string_view host) const;
  /// @brief Makes a page count that reports the current activity's pages.
  ///
  /// @return The count; it reports nothing when this object reports nothing.
  auto pages() const -> PageCount;
  /// @brief Reports that the current step ended.
  ///
  /// @param outcome How it was answered.
  void ended(StepOutcome outcome) const;
  /// @brief Reports a step the fetch does not need: it begins and ends
  /// skipped.
  ///
  /// @param step The step.
  void skipped(FetchStep step) const;
  /// @brief Reports that the attempt ended without games, or was cancelled.
  ///
  /// @param why Why.
  void failed(const Unanswered& why) const;
  /// @brief Reports that the attempt ended with games.
  void finished() const;

 private:
  /// @brief The board; null when nobody listens.
  FetchBoard* board_{};
  /// @brief The attempt every event is tagged with.
  std::uint64_t attempt_{};
};

}  // namespace libchiaking::catalog::usecases
