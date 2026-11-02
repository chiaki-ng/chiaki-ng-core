/// @file
/// @brief Declares PairedPlay, the consoles context's answer to the play
/// context's question "where and with what keys can this user play this
/// console?", and the channel through which a session reports back what it
/// learned about the console.
///
/// A session at home needs the console's address, firmware and the user's
/// pairing keys. A session over the internet needs the PSN device uid the
/// account lists the console under, which may have to be found by matching
/// the paired console to the account's console list.
#pragma once

#include "libchiaking/detail/consoles/account_console.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/host.hpp"
#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::consoles {

class KnownConsoles;

/// @brief A view of one console as one user may play it.
///
/// Only refers to the console and the user, which must outlive it; holds no
/// state of its own and is made where needed. Its queries take the
/// console's own locks and may be made from any thread.
class PairedPlay {
 public:
  /// @brief Why the console cannot be played as asked.
  enum class Refusal : std::uint8_t {
    /// @brief The console was forgotten.
    forgotten,
    /// @brief The user holds no pairing with the console.
    not_paired,
    /// @brief No address is known for the console.
    no_address,
    /// @brief An internet session needs the user signed in to PSN.
    needs_psn_sign_in,
    /// @brief The console could not be matched to exactly one console on
    /// the user's account.
    not_on_account,
    /// @brief PSN lists Remote Play as off on the matched console.
    remote_play_disabled,
    /// @brief The account's console list could not be read.
    account_unanswered,
  };

  /// @brief What a session at home connects with.
  struct HomeTarget {
    /// @brief The console's last known address.
    Host host;
    /// @brief The console's firmware family.
    Firmware firmware;
    /// @brief A copy of the user's pairing keys.
    PairingKeys keys;
  };

  /// @brief What a session over the internet connects with.
  struct InternetTarget {
    /// @brief The device uid PSN lists the console under; the main PS4
    /// placeholder for a PS4.
    DeviceUid device_uid;
    /// @brief The newest firmware of the console's generation.
    Firmware firmware;
  };

  /// @brief Where a live session reports what it learned about its console:
  /// the name it stated and a pairing an internet start made.
  ///
  /// Keeps the console and the user alive while it exists. The registry must
  /// outlive it. Used from the session's thread.
  class Reports {
   public:
    /// @brief Makes the channel for one session.
    ///
    /// @param known The registry the console belongs to; must outlive this
    ///     object.
    /// @param console The console being played.
    /// @param as The user playing it.
    Reports(KnownConsoles& known, std::shared_ptr<Console> console,
            std::shared_ptr<const users::PsUser> as) noexcept;

    /// @brief Records the name the console stated, with
    /// ConsoleName::Source::pairing, when it is usable and ranks at least
    /// as high as the held one. Blocks while the store is written.
    ///
    /// @param name The name.
    void named(std::string_view name);
    /// @brief Saves a pairing an internet start made, linked to the device
    /// uid the session reached; see ConsoleFacts::paired.
    ///
    /// Blocks while the store is written.
    ///
    /// @param paired The pairing the console granted.
    /// @param device_uid The device uid of the console reached.
    /// @return The paired console.
    /// @retval ConsoleStore::Error The store refused a write.
    [[nodiscard]] auto paired(PairedConsole paired, const DeviceUid& device_uid)
        -> Expected<std::shared_ptr<Console>, ConsoleStore::Error>;
    /// @brief Tells whether the console an internet start reached is one the
    /// user may play it as.
    ///
    /// PSN reaches a PS4 account's primary console, so for a PS4 the reached
    /// console must be a PS4 the user has paired: one with the same hardware
    /// id, or one whose name is the nickname the reached console stated. A
    /// PS5 is reached by its own device uid and is always recognised.
    ///
    /// @param reached The pairing the reached console granted.
    /// @return Whether the reached console is recognised.
    auto recognises(const PairedConsole& reached) const -> bool;

   private:
    /// @brief The registry the console belongs to.
    KnownConsoles& known_;
    /// @brief The console being played.
    std::shared_ptr<Console> console_;
    /// @brief The user playing it.
    std::shared_ptr<const users::PsUser> as_;
  };

  /// @brief Views a console as a user may play it.
  ///
  /// @param console The console; must outlive this object.
  /// @param as The user; must outlive this object.
  PairedPlay(const Console& console, const users::PsUser& as) noexcept
      : console_(console), as_(as) {}

  /// @brief What a session at home connects with.
  ///
  /// @return The address, firmware and keys.
  /// @retval Refusal::forgotten The console was forgotten.
  /// @retval Refusal::not_paired The user holds no pairing.
  /// @retval Refusal::no_address No address is known.
  [[nodiscard]] auto at_home() const -> Expected<HomeTarget, Refusal>;
  /// @brief What a session over the internet connects with, from what is
  /// already held.
  ///
  /// @return The device uid linked to the user's pairing; for a PS4 without
  ///     one, the main PS4 placeholder.
  /// @retval Refusal::forgotten The console was forgotten.
  /// @retval Refusal::needs_psn_sign_in The user is off PSN.
  /// @retval Refusal::not_on_account A PS5 whose pairing has no linked device
  ///     uid.
  [[nodiscard]] auto over_the_internet() const
      -> Expected<InternetTarget, Refusal>;
  /// @brief What a session over the internet connects with, checked against
  /// the account's console list.
  ///
  /// For a PS5, the linked device uid is confirmed when exactly one listed
  /// console of the same generation carries it. Otherwise the console is
  /// matched by name, as AccountLink::matched does for a console with no
  /// device uid; when a linked uid was held but is not listed, only a unique
  /// name match is trusted. Nothing is saved. A PS4 is answered as by the
  /// overload without a list.
  ///
  /// @param on_account The consoles the account lists.
  /// @return The matched console's device uid.
  /// @retval Refusal::forgotten The console was forgotten.
  /// @retval Refusal::needs_psn_sign_in The user is off PSN.
  /// @retval Refusal::not_on_account The linked uid is listed more than once,
  ///     or no single listed console matches.
  /// @retval Refusal::remote_play_disabled PSN lists Remote Play as off on
  ///     the matched console.
  [[nodiscard]] auto over_the_internet(
      std::span<const AccountConsole> on_account) const
      -> Expected<InternetTarget, Refusal>;
  /// @brief What a session over the internet connects with, reading the
  /// account's console list for a PS5.
  ///
  /// Reading the list also links the user's paired PS5 consoles; see
  /// KnownConsoles::consoles_on_account. Blocks for the PSN request.
  ///
  /// @param known The registry.
  /// @param console The console.
  /// @param as The user.
  /// @param stop Cancels the request.
  /// @return The matched console's device uid.
  /// @retval Refusal::account_unanswered The account's console list could not
  ///     be read.
  /// @retval Refusal Any refusal of over_the_internet().
  [[nodiscard]] static auto found_on_account(KnownConsoles& known,
                                             const Console& console,
                                             users::PsUser& as,
                                             const platform::StopToken& stop)
      -> Expected<InternetTarget, Refusal>;

 private:
  /// @brief The console.
  const Console& console_;
  /// @brief The user.
  const users::PsUser& as_;
};

}  // namespace libchiaking::consoles
