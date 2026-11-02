/// @file
/// @brief Declares CredentialStore, the persistence port for signed-in users,
/// and the rows it holds.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/account_context.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/credentials.hpp"
#include "libchiaking/detail/users/device_id.hpp"
#include "libchiaking/detail/users/sign_in.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::users {

/// @brief The storefront region of an account.
struct Region {
  /// @brief The country code.
  std::string country;
  /// @brief The language code.
  std::string language;

  /// @brief Compares both fields.
  friend auto operator==(const Region&, const Region&) -> bool = default;
};

/// @brief Everything persisted for a user who signed in to PSN.
///
/// The row is full of secrets: the NPSSO, access tokens and refresh tokens.
/// Each is a Credential, which keeps it in wiping memory, so the row is
/// move-only and clone() is the only way to duplicate it. An implementation of
/// CredentialStore is responsible for protecting the row at rest.
struct SavedSignIn {
  /// @brief The clock the row's times are expressed in.
  using Time = std::chrono::system_clock::time_point;

  /// @brief The app token, which authorises the store, profile and library
  /// calls.
  struct StoreAccess {
    /// @brief The token. Secret.
    AppToken token;
    /// @brief When the token stops being accepted.
    Time expires_at;
    /// @brief When the token was issued, if known.
    std::optional<Time> obtained_at{};
  };

  /// @brief The refresh token that renews the app token without the NPSSO.
  struct StoreRefresh {
    /// @brief The token. Secret.
    AppRefreshToken token;
    /// @brief When the token was issued, if known.
    std::optional<Time> obtained_at{};
    /// @brief When the token stops being accepted, if PSN said.
    std::optional<Time> expires_at{};
  };

  /// @brief The Remote Play token, which authorises listing the account's
  /// consoles and playing over the internet.
  struct RemotePlayAccess {
    /// @brief The token. Secret.
    BearerToken token;
    /// @brief When the token stops being accepted.
    Time expires_at;
    /// @brief When the token was issued, if known.
    std::optional<Time> obtained_at{};
  };

  /// @brief The refresh token that renews the Remote Play token.
  struct RemotePlayRefresh {
    /// @brief The token. Secret.
    RemotePlayRefreshToken token;
    /// @brief When the token was issued, if known.
    std::optional<Time> obtained_at{};
    /// @brief When the token stops being accepted, if PSN said.
    std::optional<Time> expires_at{};
  };

  /// @brief The account the sign-in belongs to. The row's key.
  AccountId account;
  /// @brief The account's online id, shown as the user's name.
  std::string online_id;
  /// @brief The NPSSO, the seed every other token is obtained from. Secret.
  Npsso npsso;
  /// @brief The app token, when one is held.
  std::optional<StoreAccess> store{};
  /// @brief The app refresh token, when one is held.
  std::optional<StoreRefresh> store_refresh{};
  /// @brief The Remote Play token, when one is held.
  std::optional<RemotePlayAccess> remote_play{};
  /// @brief The Remote Play refresh token, when one is held.
  std::optional<RemotePlayRefresh> remote_play_refresh{};
  /// @brief The account's storefront region, once it has been learned.
  std::optional<Region> region{};
  /// @brief What the app token stated about the account's country, locale and
  /// age.
  AccountContext context{};
  /// @brief The device id this installation presents to PSN for the account.
  /// A row loaded without one is given a new one and saved again.
  std::optional<DeviceId> device{};
  /// @brief When the NPSSO was supplied, if known.
  std::optional<Time> npsso_obtained_at{};
  /// @brief When the NPSSO stops being accepted, if the application said.
  std::optional<Time> npsso_expires_at{};

  /// @brief Makes a deliberate copy of the row, secrets included.
  ///
  /// @return An independent row with its own copies of the secrets.
  auto clone() const -> SavedSignIn {
    SavedSignIn copy{account, online_id, npsso.clone()};
    if (store)
      copy.store.emplace(store->token.clone(), store->expires_at,
                         store->obtained_at);
    if (store_refresh)
      copy.store_refresh.emplace(store_refresh->token.clone(),
                                 store_refresh->obtained_at,
                                 store_refresh->expires_at);
    if (remote_play)
      copy.remote_play.emplace(remote_play->token.clone(),
                               remote_play->expires_at,
                               remote_play->obtained_at);
    if (remote_play_refresh)
      copy.remote_play_refresh.emplace(remote_play_refresh->token.clone(),
                                       remote_play_refresh->obtained_at,
                                       remote_play_refresh->expires_at);
    copy.region = region;
    copy.context = context;
    if (device) copy.device.emplace(device->clone());
    copy.npsso_obtained_at = npsso_obtained_at;
    copy.npsso_expires_at = npsso_expires_at;
    return copy;
  }
};

/// @brief What is persisted for a user who has no PSN sign-in.
///
/// Such a user is known only by an account id and a name, which is enough to
/// pair with a console and play on the home network.
///
/// @see SignedInUsers::add_home_user
struct SavedHomeUser {
  /// @brief The account the user pairs as. The row's key.
  AccountId account;
  /// @brief The name shown for the user.
  std::string name;

  /// @brief Compares both fields.
  friend auto operator==(const SavedHomeUser&, const SavedHomeUser&)
      -> bool = default;
};

/// @brief Port through which SignedInUsers and PsUser persist users.
///
/// The application implements this over its own storage. Saves happen whenever
/// a user signs in or a token is renewed, on whichever thread caused it, so an
/// implementation must be thread safe.
///
/// @see SignedInUsers
/// @see testing::InMemoryCredentialStore
class CredentialStore {
 public:
  /// @brief Why a store call failed.
  enum class Error : std::uint8_t {
    /// @brief The storage could not be read or written.
    unavailable,
  };

  /// @brief Allows deletion through the interface.
  virtual ~CredentialStore() = default;

  /// @brief Loads every saved PSN sign-in.
  ///
  /// Called by SignedInUsers::welcome_back.
  ///
  /// @return The saved sign-ins, or Error::unavailable when storage cannot be
  ///     read.
  [[nodiscard]] virtual auto load_saved_users()
      -> Expected<std::vector<SavedSignIn>, Error> = 0;
  /// @brief Loads every saved user that has no PSN sign-in.
  ///
  /// Called by SignedInUsers::welcome_back.
  ///
  /// @return The saved home users, or Error::unavailable when storage cannot
  ///     be read.
  [[nodiscard]] virtual auto load_home_users()
      -> Expected<std::vector<SavedHomeUser>, Error> = 0;
  /// @brief Writes a sign-in, replacing any row for the same account.
  ///
  /// @param user The row to persist. It holds secrets.
  /// @return Nothing on success, or Error::unavailable when the write failed.
  [[nodiscard]] virtual auto save(const SavedSignIn& user)
      -> Expected<void, Error> = 0;
  /// @brief Writes a user that has no PSN sign-in, replacing any row for the
  /// same account.
  ///
  /// @param user The row to persist.
  /// @return Nothing on success, or Error::unavailable when the write failed.
  [[nodiscard]] virtual auto save(const SavedHomeUser& user)
      -> Expected<void, Error> = 0;
  /// @brief Deletes everything saved for an account.
  ///
  /// Called by SignedInUsers::sign_out.
  ///
  /// @param account The account to delete.
  /// @return Nothing on success, or Error::unavailable when the delete
  ///     failed.
  [[nodiscard]] virtual auto remove(AccountId account)
      -> Expected<void, Error> = 0;
};

}  // namespace libchiaking::users
