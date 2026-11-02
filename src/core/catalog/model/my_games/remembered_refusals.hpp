/// @file
/// @brief Declares RememberedRefusals, the refusals from PS's streaming
/// service that still apply to a title while MyGames is built.
#pragma once

#include <libchiaking/detail/catalog/stream_refusal.hpp>
#include <libchiaking/detail/catalog/title.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/detail/users/membership.hpp>

#include <optional>
#include <span>

namespace libchiaking::catalog {

/// @brief The refusals remembered for one account, judged against the
/// user's present tier and one moment.
///
/// It views the refusals without owning them; the span's storage must
/// outlive it. It holds no mutable state and is used on the thread that
/// assembles MyGames.
class RememberedRefusals {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = StreamRefusal::Time;

  /// @brief Views the refusals to judge.
  ///
  /// @param account The account the games are for; refusals of other
  ///     accounts are ignored.
  /// @param refusals Every remembered refusal. Must outlive this object.
  /// @param tier The user's present tier; std::nullopt when unknown, in
  ///     which case each refusal is judged against its own recorded tier.
  /// @param now The moment to judge expiry at.
  RememberedRefusals(AccountId account, std::span<const StreamRefusal> refusals,
                     std::optional<users::Tier> tier, Time now) noexcept;

  /// @brief The latest refusal that still keeps a title from launching.
  ///
  /// A refusal counts when it is for this account, the title is known as
  /// its product, and StreamRefusal::still_stands holds for the present
  /// tier and holding.
  ///
  /// @param title The title.
  /// @param holding How the user holds the title now.
  /// @return The standing refusal with the latest refused_at time, or
  ///     std::nullopt when none stands.
  auto standing_against(const Title& title,
                        const StreamRefusal::Holding& holding) const
      -> std::optional<StreamRefusal>;

 private:
  /// @brief The account the games are for.
  AccountId account_;
  /// @brief The refusals, not owned.
  std::span<const StreamRefusal> refusals_;
  /// @brief The user's present tier, when known.
  std::optional<users::Tier> tier_;
  /// @brief The moment expiry is judged at; wall-clock milliseconds.
  Time now_;
};

}  // namespace libchiaking::catalog
