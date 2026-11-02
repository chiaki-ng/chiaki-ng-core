/// @file
/// @brief Declares cache::HeldPlays, the play history held in memory for each
/// account.
#pragma once

#include <libchiaking/detail/catalog/played_game.hpp>
#include <libchiaking/detail/users/account_id.hpp>

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief Every account's play history and when it was read.
///
/// Not synchronised: its owner holds one lock around every call, so that a
/// change here and a change to the owner's other parts happen together.
class HeldPlays {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = std::chrono::sys_time<std::chrono::milliseconds>;
  /// @brief A held play history.
  using PlayedPtr = std::shared_ptr<const std::vector<PlayedGame>>;

  /// @brief Whether a play history read at a time is within its lifetime.
  ///
  /// @param read_at When it was read.
  /// @param now The wall-clock time now.
  /// @return True when it was read less than six hours before now, and not
  ///     after it.
  static auto is_fresh(Time read_at, Time now) -> bool;

  /// @brief The account's play history, when it is within its lifetime.
  ///
  /// @param account The account.
  /// @param now The wall-clock time now.
  /// @return The history, or nullptr when none is held or it is too old.
  auto fresh(AccountId account, Time now) const -> PlayedPtr;
  /// @brief Holds a play history.
  ///
  /// @param account The account.
  /// @param games The history.
  /// @param read_at When it was read.
  void hold(AccountId account, PlayedPtr games, Time read_at);
  /// @brief Drops every account's play history.
  void drop_everyone();
  /// @brief Forgets the play history held for an account.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief A play history and when it was read.
  struct Read {
    /// @brief The history.
    PlayedPtr games{};
    /// @brief When it was read.
    Time read_at{};
  };

  /// @brief The histories, by account id.
  std::map<std::uint64_t, Read> played_;
};

}  // namespace libchiaking::catalog::cache
