/// @file
/// @brief Declares CouchAccounts, the PSN accounts the couch protocol joins
/// pads as and the ones the console could not resolve.
#pragma once

#include "libchiaking/detail/users/account_id.hpp"
#include "src/core/net/protocol/remote_play/stream/controller.hpp"
#include "src/core/utils/text/base64.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::play {

/// @brief The base64 PSN account each pad will join a PS5 as, and the
/// accounts the console said it cannot resolve, which are never joined again
/// in the stream.
///
/// Owned by the console half of the couch protocol and used on its thread; it
/// is not synchronised.
class CouchAccounts {
 public:
  /// @brief Remembers the account a pad will join as, base64-encoded.
  ///
  /// @param pad The pad number, below Pad::count.
  /// @param account The account.
  void remember(std::uint8_t pad, const AccountId& account) {
    std::string encoded;
    text::base64::append(encoded, account.bytes(),
                         text::base64::Alphabet::standard);
    accounts_[pad] = std::move(encoded);
  }

  /// @brief Reads the account a pad will join as.
  ///
  /// @param pad The pad number, below Pad::count.
  /// @return The base64 account; empty when unset.
  auto account(std::uint8_t pad) const noexcept -> const std::string& {
    return accounts_[pad];
  }

  /// @brief Reports whether a pad may join with its remembered account.
  ///
  /// @param pad The pad number, below Pad::count.
  /// @return `false` without an account or for one the console could not
  ///     resolve.
  auto joinable(std::uint8_t pad) const -> bool {
    const auto& account = accounts_[pad];
    return !account.empty() &&
           std::find(unresolvable_.begin(), unresolvable_.end(), account) ==
               unresolvable_.end();
  }

  /// @brief Remembers that the console cannot resolve an account.
  ///
  /// @param account The base64 account.
  void unresolvable(const std::string& account) {
    if (std::find(unresolvable_.begin(), unresolvable_.end(), account) ==
        unresolvable_.end())
      unresolvable_.push_back(account);
  }

 private:
  /// @brief The base64 account each pad will join as; empty when unset.
  std::array<std::string, net::protocol::remote_play::Pad::count> accounts_{};
  /// @brief Accounts the console said it cannot resolve.
  std::vector<std::string> unresolvable_;
};

}  // namespace libchiaking::play
