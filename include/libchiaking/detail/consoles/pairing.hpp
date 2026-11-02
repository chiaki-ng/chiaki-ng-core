/// @file
/// @brief Declares the values pairing consumes and produces: PairingPin,
/// PairingKeys, PairedConsole, progress and failure.
#pragma once

#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/detail/internet_route.hpp"
#include "libchiaking/detail/progress.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/detail/wire_sizes.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::consoles {

/// @brief The eight-digit PIN a console displays while it waits to be paired.
///
/// The PIN is a secret. The type is move-only and overwrites its digits when it
/// is destroyed or moved from, so a PIN exists in one place from the moment it
/// is parsed until pairing consumes it.
class PairingPin {
 public:
  /// @brief Number of digits in a PIN.
  static constexpr std::size_t digit_count = 8;
  /// @brief Largest value a PIN can have.
  static constexpr std::uint32_t maximum = 99'999'999;

  /// @brief Reads a PIN from the text the user typed.
  ///
  /// @param digits Exactly digit_count ASCII digits, with no spaces.
  /// @return The PIN, or nothing for any other text.
  [[nodiscard]] static auto parse(std::string_view digits) noexcept
      -> std::optional<PairingPin>;
  /// @brief Makes a PIN from a number.
  ///
  /// @param digits The PIN as a number. Leading zeros are implied.
  /// @return The PIN, or nothing when the number is above maximum.
  [[nodiscard]] static auto create(std::uint32_t digits) noexcept
      -> std::optional<PairingPin>;

  /// @brief Takes the PIN from another object, whose digits are wiped.
  PairingPin(PairingPin&&) noexcept = default;
  /// @brief Replaces this PIN with another object's, whose digits are wiped.
  auto operator=(PairingPin&&) noexcept -> PairingPin& = default;

  /// @brief The PIN as a number.
  ///
  /// @return The digits read as one decimal number, at most maximum. The
  ///     copy is plain memory; spend it at once.
  auto value() const noexcept -> std::uint32_t;

 private:
  explicit PairingPin(std::uint32_t digits) noexcept;

  SecretArray<sizeof(std::uint32_t)> digits_;
};

/// @brief The secret keys a console issues when it is paired.
///
/// Whoever holds them can wake the console and open Remote Play sessions as the
/// paired account. They are kept in wiping storage, the type is move-only, and
/// a copy is only made by calling clone().
class PairingKeys {
 public:
  /// @brief Size of the registration key in bytes.
  static constexpr std::size_t registration_key_size = rp_registration_key_size;
  /// @brief Size of the Remote Play key in bytes.
  static constexpr std::size_t remote_play_key_size = rp_key_size;

  /// @brief Adopts keys a console issued or a ConsoleStore loaded.
  ///
  /// @param registration_key The key that identifies the pairing to the
  ///     console. Also the source of the wake credential.
  /// @param remote_play_key The Remote Play key the console issued, used when
  ///     a session is opened.
  /// @param key_type The key type number the console stated alongside the
  ///     keys.
  /// @return The keys, or nothing when either key is all zero bytes.
  [[nodiscard]] static auto create(
      std::span<const std::byte, registration_key_size> registration_key,
      std::span<const std::byte, remote_play_key_size> remote_play_key,
      std::uint32_t key_type) noexcept -> std::optional<PairingKeys>;

  /// @brief Moves the keys; the source's bytes are wiped.
  PairingKeys(PairingKeys&&) noexcept = default;
  /// @brief Move-assigns the keys; the source's bytes are wiped.
  auto operator=(PairingKeys&&) noexcept -> PairingKeys& = default;

  /// @brief Makes a deliberate second copy of the keys, which wipes itself
  /// independently.
  ///
  /// @return An independent copy that wipes itself separately.
  auto clone() const noexcept -> PairingKeys;

  /// @brief The registration key. The view is valid while this object lives
  /// and is not moved from.
  ///
  /// @return A view of the registration key.
  auto registration_key() const noexcept
      -> std::span<const std::byte, registration_key_size>;
  /// @brief The Remote Play key. The view is valid while this object lives
  /// and is not moved from.
  ///
  /// @return A view of the Remote Play key.
  auto remote_play_key() const noexcept
      -> std::span<const std::byte, remote_play_key_size>;
  /// @brief The key type number the console stated alongside the keys.
  ///
  /// @return The key type number.
  auto key_type() const noexcept -> std::uint32_t { return key_type_; }

 private:
  PairingKeys() noexcept = default;

  SecretArray<registration_key_size> registration_key_;
  SecretArray<remote_play_key_size> remote_play_key_;
  std::uint32_t key_type_{};
};

/// @brief What a successful pairing exchange yields, before it is saved.
struct PairedConsole {
  /// @brief The console's MAC address, as it stated in its reply.
  HardwareId hardware_id;
  /// @brief The firmware family the console reported, or the one it was asked
  /// as.
  Firmware firmware;
  /// @brief The nickname the console stated, when it gave a usable one.
  std::optional<ConsoleName> name;
  /// @brief The secret keys the console issued.
  PairingKeys keys;
};

/// @brief The coarse stages of pairing over the internet.
///
/// @see PairingWatcher::on_stage
enum class InternetPairingStage : std::uint8_t {
  /// @brief The attempt has begun.
  starting,
  /// @brief A PSN session for reaching the console is being opened.
  opening_session,
  /// @brief The console is being woken through PSN.
  waking_console,
  /// @brief A connection to the console is being established.
  connecting,
  /// @brief The pairing exchange with the console is under way.
  pairing,
};

/// @brief The steps of a pairing attempt, at home or over the internet, in the
/// order they occur.
///
/// @see PairingProgress
enum class PairingStep : std::uint8_t {
  /// @brief The attempt has begun. This is the initial value.
  starting,
  /// @brief Internet pairing only: a PSN session is being opened.
  opening_account_session,
  /// @brief Internet pairing only: the console is being woken and asked to
  /// join.
  waking_console,
  /// @brief A connection to the console is being opened.
  reaching_console,
  /// @brief The pairing request is being sent.
  sending_request,
  /// @brief The request was sent and the console's answer is awaited.
  waiting_for_answer,
  /// @brief The console accepted; the pairing is being written to the
  /// ConsoleStore.
  saving,
};

/// @brief One report of how far a pairing attempt has got.
///
/// @see PairingWatcher::on_progress
struct PairingProgress {
  /// @brief The step just entered.
  PairingStep step{PairingStep::starting};
  /// @brief Time since the attempt began.
  std::chrono::microseconds elapsed{};
  /// @brief Which attempt this is, counting from 1.
  std::uint32_t attempt{1};
  /// @brief What the attempt is waiting for, when it is waiting.
  std::optional<progress::Wait> wait{};
  /// @brief Internet pairing only: how route finding to the console is going.
  std::optional<progress::InternetRoute> route{};

  /// @brief The attempt, elapsed time and wait in the form shared with other
  /// progress reports. The position is never set.
  ///
  /// @return The Status with no position set.
  constexpr auto status() const noexcept -> progress::Status {
    return {std::nullopt, attempt, elapsed, wait};
  }

  /// @brief Compares every field.
  friend constexpr auto operator==(const PairingProgress&,
                                   const PairingProgress&) noexcept
      -> bool = default;
};

/// @brief Why a pairing attempt failed, with whatever detail the failing party
/// gave.
struct PairingFailure {
  /// @brief The kinds of failure.
  enum class Reason : std::uint8_t {
    /// @brief The address is an IPv6 literal and the console is a PS4.
    address_not_for_generation,
    /// @brief The console could not be reached, or no address is known for
    /// it.
    unreachable,
    /// @brief The console rejected the request. A wrong PIN is the usual
    /// cause.
    refused,
    /// @brief A cryptographic step failed, usually decrypting the console's
    /// reply.
    unreadable_reply,
    /// @brief The request could not be built or the reply did not hold a
    /// usable hardware id and keys.
    malformed,
    /// @brief The console did not finish in time.
    timed_out,
    /// @brief The stop token was triggered.
    cancelled,
    /// @brief The pairing exchange was not in a state to continue.
    busy,
    /// @brief The console accepted but the ConsoleStore refused to save the
    /// pairing.
    not_saved,
    /// @brief PSN reports Remote Play as switched off on the console.
    remote_play_disabled,
    /// @brief Internet pairing was asked for but no InternetPairing is
    /// installed.
    internet_pairing_missing,
    /// @brief The user has no usable PSN sign-in; see
    /// PairingFailure::unanswered.
    unanswered,
    /// @brief Something pairing needs is not provided in this set-up. This
    /// is the default value.
    not_available,
    /// @brief The console is a PS4 on system software below 7.0, which
    /// pairs by online id, and the user has no PSN account.
    needs_online_id,
    /// @brief Internet pairing was refused because a play session is
    /// running.
    session_playing,
  };

  /// @brief What went wrong.
  Reason reason{Reason::not_available};
  /// @brief The HTTP status of the console's reply, when it sent one.
  std::optional<std::uint16_t> http_status{};
  /// @brief The application reason code the console gave for refusing, when
  /// it gave one.
  std::optional<std::uint32_t> ps_reason{};
  /// @brief How far internet pairing had got. Not set for pairing at home.
  std::optional<InternetPairingStage> internet_stage{};
  /// @brief The PSN failure behind Reason::unanswered.
  std::optional<users::Unanswered> unanswered{};
};

/// @brief Receives progress while a pairing attempt runs.
///
/// Both members do nothing by default, so override only what is needed. During
/// KnownConsoles::pair_at_home they are called on the calling thread. During
/// internet pairing the calling thread is not guaranteed, so an implementation
/// should be thread safe.
class PairingWatcher {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~PairingWatcher() = default;
  /// @brief Called when internet pairing enters a new stage. Never called
  /// while pairing at home.
  ///
  /// The attempt waits for the call to return, so it should be brief and
  /// must not call back into the KnownConsoles that is pairing.
  ///
  /// @param stage The stage just entered. Stages arrive in the order of
  ///     InternetPairingStage and none repeats within one attempt.
  virtual void on_stage(InternetPairingStage stage) {
    static_cast<void>(stage);
  }
  /// @brief Called when a pairing attempt enters a new step, and once more
  /// with PairingStep::saving before the result is stored.
  ///
  /// Steps only move forward: each report names a later PairingStep than
  /// the one before, and a step the attempt passed through too quickly to
  /// be seen is still reported, in order. Pairing at home never reports the
  /// internet-only steps. The attempt waits for the call to
  /// return, so it should be brief and must not call back into the
  /// KnownConsoles that is pairing. Pairing at home calls it on the thread
  /// that called KnownConsoles::pair_at_home.
  ///
  /// @param progress The step just entered, the time since the attempt
  ///     began and, for PairingStep::waiting_for_answer, what is awaited.
  ///     The reference is valid only during the call.
  virtual void on_progress(const PairingProgress& progress) {
    static_cast<void>(progress);
  }
};

}  // namespace libchiaking::consoles
