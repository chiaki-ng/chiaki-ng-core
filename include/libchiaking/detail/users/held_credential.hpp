/// @file
/// @brief Declares the vocabulary for inspecting a user's credentials without
/// exposing them: CredentialKind, CredentialUse and HeldCredential.
#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace libchiaking::users {

/// @brief The credentials a signed-in user can hold.
enum class CredentialKind : std::uint8_t {
  /// @brief The NPSSO, from which every other credential is obtained.
  sign_in_seed,
  /// @brief The app token, which authorises the store, profile and library
  /// calls.
  app_token,
  /// @brief The refresh token that renews the app token.
  app_refresh_token,
  /// @brief The Remote Play token, which authorises account console listing
  /// and internet play.
  remote_play_token,
  /// @brief The refresh token that renews the Remote Play token.
  remote_play_refresh_token,
  /// @brief The token that authorises reading the owned-games library.
  library_token,
  /// @brief The token that authorises checkout on the older PlayStation Now
  /// store service.
  old_service_checkout_token,
};

/// @brief Every CredentialKind, in the order PsUser::held_credentials reports
/// them.
inline constexpr std::array every_credential{
    CredentialKind::sign_in_seed,
    CredentialKind::app_token,
    CredentialKind::app_refresh_token,
    CredentialKind::remote_play_token,
    CredentialKind::remote_play_refresh_token,
    CredentialKind::library_token,
    CredentialKind::old_service_checkout_token};

/// @brief What a credential is used for.
enum class CredentialUse : std::uint8_t {
  /// @brief Obtaining other tokens.
  exchanging_tokens,
  /// @brief Authorising a cloud streaming session.
  cloud_streaming_authorisation,
  /// @brief Opening a session with the older PlayStation Now store service.
  old_service_store,
  /// @brief Fetching the user's profile.
  profile,
  /// @brief Fetching the user's PlayStation Plus membership.
  membership,
  /// @brief Checking a title against the storefront.
  storefront_check,
  /// @brief Listing the titles installed on the user's consoles.
  console_titles,
  /// @brief Asking PSN to start a title on a console.
  starting_a_title,
  /// @brief Looking up the concepts of owned copies.
  owned_copy_concepts,
  /// @brief Renewing the app token.
  renewing_the_app_token,
  /// @brief Listing the consoles on the account.
  account_consoles,
  /// @brief Reaching a console over the internet.
  internet_play,
  /// @brief Renewing the Remote Play token.
  renewing_the_remote_play_token,
  /// @brief Reading the owned-games library.
  owned_library,
  /// @brief Checking out on the older PlayStation Now store service.
  old_service_checkout,
};

/// @brief A description of one credential a user holds or could hold. It never
/// contains the secret.
///
/// @see PsUser::held_credentials
/// @see PsUser::reveal
struct HeldCredential {
  /// @brief The clock the times are expressed in.
  using Time = std::chrono::system_clock::time_point;

  /// @brief Where a credential lives.
  enum class Kept : std::uint8_t {
    /// @brief It is part of the saved sign-in and survives a restart.
    with_the_user,
    /// @brief It exists only in memory and is obtained again when needed.
    in_memory,
  };

  /// @brief What is known about whether a credential works.
  enum class State : std::uint8_t {
    /// @brief Held and, as far as is known, accepted by PSN.
    usable,
    /// @brief Held but past its expiry.
    expired,
    /// @brief PSN refused the credential or the request to obtain it.
    refused,
    /// @brief Not held and no attempt to obtain it has been refused.
    never_obtained,
    /// @brief Held and not past any known expiry, but not confirmed against
    /// PSN. The seed and the refresh tokens report this.
    unverified,
  };

  /// @brief Which credential this describes.
  CredentialKind kind{};
  /// @brief Whether the user holds the credential now.
  bool held{};
  /// @brief Where the credential lives.
  Kept kept{Kept::in_memory};
  /// @brief What is known about whether it works.
  State state{State::never_obtained};
  /// @brief When the credential was obtained, if known.
  std::optional<Time> obtained_at{};
  /// @brief When the credential expires, if known.
  std::optional<Time> expires_at{};
  /// @brief The OAuth scopes the credential was issued with, when PSN stated
  /// them. Empty otherwise.
  std::string scopes{};
  /// @brief The HTTP status of the last reply to a request that used the
  /// credential or, failing that, to the last request to obtain it.
  std::optional<std::uint16_t> last_reply_status{};
  /// @brief Everything the credential is used for.
  std::vector<CredentialUse> used_for{};

  /// @brief Compares every field.
  friend auto operator==(const HeldCredential&, const HeldCredential&)
      -> bool = default;
};

/// @brief Why PsUser::reveal did not hand over a secret.
struct NotRevealed {
  /// @brief The kinds of refusal.
  enum class Reason : std::uint8_t {
    /// @brief The user does not hold that credential.
    not_held,
    /// @brief The user has no PSN sign-in.
    signed_out,
  };

  /// @brief Why the secret was not revealed.
  Reason reason{Reason::not_held};
};

}  // namespace libchiaking::users
