/// @file
/// @brief Declares QrSignInState, where a password-free sign-in by QR code
/// stands, the observer told of every change to it, and QrSignInOptions, what
/// the application may choose about it.
#pragma once

#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/detail/users/sign_in.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::users {

/// @brief One step of SignedInUsers::sign_in_with_qr.
///
/// A sign-in starts in QrSignInStep::preparing, shows a code in
/// QrSignInStep::awaiting_approval and, once the code is approved, passes
/// QrSignInStep::signing_in. A code that expires before it is approved sends
/// the sign-in back to QrSignInStep::preparing for a replacement, with the
/// attempt counted up. It ends in QrSignInStep::signed_in or
/// QrSignInStep::failed.
enum class QrSignInStep : std::uint8_t {
  /// @brief The registration, the application token and a code are being
  /// obtained.
  preparing,
  /// @brief A code is shown and PSN is asked, at the interval it set, whether
  /// it has been approved.
  awaiting_approval,
  /// @brief The code was approved and the account is being signed in.
  signing_in,
  /// @brief The account is signed in and held by the registry.
  signed_in,
  /// @brief The sign-in ended without an account; QrSignInState::failure says
  /// why.
  failed,
};

/// @brief Where a QR sign-in stands: a snapshot that never changes once made.
///
/// Follow every change with the QrSignInObserver given to
/// SignedInUsers::sign_in_with_qr. The approval URL and the code are meant to
/// be shown, but only to the person signing in, so both are held in wiping
/// memory and the snapshot is move-only; clone() makes the one deliberate
/// copy.
class QrSignInState {
 public:
  /// @brief The state before anything was obtained: QrSignInStep::preparing,
  /// attempt 0.
  QrSignInState() noexcept = default;
  /// @brief Makes a snapshot.
  ///
  /// @param attempt The code the snapshot belongs to, counted from 1 for each
  ///     sign-in.
  /// @param step The step reached.
  /// @param approval_url The address to show as the QR code; only for
  ///     QrSignInStep::awaiting_approval.
  /// @param user_code The six digits the approving device shows beside the
  ///     code; only for QrSignInStep::awaiting_approval.
  /// @param expires_at When the code stops being accepted, by the wall
  ///     clock; only for QrSignInStep::awaiting_approval.
  /// @param failure Why the sign-in ended without an account; only for
  ///     QrSignInStep::failed.
  QrSignInState(std::uint32_t attempt, QrSignInStep step,
                SecretString approval_url, SecretString user_code,
                std::optional<std::chrono::system_clock::time_point> expires_at,
                std::optional<SignInErrorKind> failure);
  /// @brief Not copyable; use clone().
  QrSignInState(const QrSignInState&) = delete;
  /// @brief Not copy-assignable; use clone().
  auto operator=(const QrSignInState&) -> QrSignInState& = delete;
  /// @brief Takes over another snapshot; the other is left empty.
  QrSignInState(QrSignInState&&) noexcept = default;
  /// @brief Takes over another snapshot; the other is left empty.
  ///
  /// @return This snapshot.
  auto operator=(QrSignInState&&) noexcept -> QrSignInState& = default;
  /// @brief Releases the snapshot, wiping the URL and the code.
  ~QrSignInState() = default;

  /// @brief Makes an independent copy, the URL and the code into new wiping
  /// strings.
  ///
  /// @return The copy.
  auto clone() const -> QrSignInState;

  /// @brief The code the snapshot belongs to.
  ///
  /// @return The attempt, counted from 1 and raised for every replacement
  ///     code; 0 before the first.
  auto attempt() const noexcept -> std::uint32_t { return attempt_; }
  /// @brief The step reached.
  ///
  /// @return The step.
  auto step() const noexcept -> QrSignInStep { return step_; }
  /// @brief The address to show as the QR code.
  ///
  /// @return The URL, valid while this snapshot lives; empty unless the step
  ///     is QrSignInStep::awaiting_approval.
  auto approval_url() const noexcept -> std::string_view {
    return approval_url_.view();
  }
  /// @brief The six digits shown beside the code, leading zeroes kept.
  ///
  /// @return The code, valid while this snapshot lives; empty unless the
  ///     step is QrSignInStep::awaiting_approval.
  auto user_code() const noexcept -> std::string_view {
    return user_code_.view();
  }
  /// @brief When the code shown stops being accepted.
  ///
  /// @return The wall-clock time, or nothing unless the step is
  ///     QrSignInStep::awaiting_approval.
  auto expires_at() const noexcept
      -> std::optional<std::chrono::system_clock::time_point> {
    return expires_at_;
  }
  /// @brief Why the sign-in ended without an account.
  ///
  /// @return The kind of failure for QrSignInStep::failed; nothing otherwise.
  auto failure() const noexcept -> std::optional<SignInErrorKind> {
    return failure_;
  }

  /// @brief Compares two snapshots field by field.
  ///
  /// @param left One snapshot.
  /// @param right The other snapshot.
  /// @return True when every field is the same.
  friend auto operator==(const QrSignInState& left, const QrSignInState& right)
      -> bool {
    return left.attempt_ == right.attempt_ && left.step_ == right.step_ &&
           left.approval_url() == right.approval_url() &&
           left.user_code() == right.user_code() &&
           left.expires_at_ == right.expires_at_ &&
           left.failure_ == right.failure_;
  }

 private:
  /// @brief The code the snapshot belongs to.
  std::uint32_t attempt_{};
  /// @brief The step reached.
  QrSignInStep step_{QrSignInStep::preparing};
  /// @brief The address shown as the QR code, which carries the code.
  SecretString approval_url_{};
  /// @brief The six digits shown beside the code.
  SecretString user_code_{};
  /// @brief When the code stops being accepted.
  std::optional<std::chrono::system_clock::time_point> expires_at_{};
  /// @brief Why the sign-in ended without an account.
  std::optional<SignInErrorKind> failure_{};
};

/// @brief Told every change of a QR sign-in's state, in order.
///
/// Called on the thread that called SignedInUsers::sign_in_with_qr. It must
/// return quickly and must not start another sign-in.
using QrSignInObserver = std::function<void(const QrSignInState&)>;

/// @brief What the application may choose about SignedInUsers::sign_in_with_qr;
/// every choice is optional.
///
/// Choices are made by chaining, as in
/// `QrSignInOptions{}.device_name("Living room Switch")`. Options left alone
/// keep PlayStation's sign-in defaults.
class QrSignInOptions {
 public:
  /// @brief The name the approving device shows when none was chosen.
  static constexpr std::string_view default_device_name = "my device";
  /// @brief Longest device name accepted, in bytes of UTF-8.
  static constexpr std::size_t maximum_device_name_bytes = 64;

  /// @brief Leaves every choice at its default.
  QrSignInOptions() = default;

  /// @brief Chooses the name the approving device shows for this device.
  ///
  /// The name is checked when the sign-in starts, not here: one that
  /// device_name_usable() refuses makes SignedInUsers::sign_in_with_qr fail
  /// with SignInErrorKind::invalid_request before anything is sent.
  ///
  /// @param name The name, in UTF-8.
  /// @return These options, for chaining.
  auto device_name(std::string_view name) & -> QrSignInOptions& {
    device_name_ = std::string{name};
    return *this;
  }
  /// @brief Chooses the name the approving device shows for this device, on a
  /// temporary.
  ///
  /// @param name The name, in UTF-8.
  /// @return These options, for chaining.
  /// @see device_name(std::string_view) &
  auto device_name(std::string_view name) && -> QrSignInOptions&& {
    device_name_ = std::string{name};
    return std::move(*this);
  }
  /// @brief The name the approving device will show.
  ///
  /// @return The chosen name, or QrSignInOptions::default_device_name when none
  ///     was chosen; valid while these options live and are not changed.
  auto device_name() const noexcept -> std::string_view {
    return device_name_ ? std::string_view{*device_name_} : default_device_name;
  }
  /// @brief Whether the device name can be sent.
  ///
  /// @return True when the name is valid UTF-8, not empty, at most
  ///     QrSignInOptions::maximum_device_name_bytes long and free of control
  ///     characters.
  auto device_name_usable() const noexcept -> bool;

  /// @brief Compares every choice.
  ///
  /// @param left One set of options.
  /// @param right The other set of options.
  /// @return True when every choice is the same.
  friend auto operator==(const QrSignInOptions& left,
                         const QrSignInOptions& right) -> bool = default;

 private:
  /// @brief The chosen device name, if one was chosen.
  std::optional<std::string> device_name_{};
};

}  // namespace libchiaking::users
