/// @file
/// @brief Declares SessionCodec, the encoder of the Remote Play session
/// request and the reader of the console's reply.
///
/// Starting a stream begins with `GET /sie/ps5/rp/sess/init` (the path varies
/// by console variant). The request proves registration with the
/// `RP-Registkey` header; a console that accepts answers 200 with a base64
/// `RP-Nonce` from which the session's AuthMaterial is derived. A console on
/// a different RP version answers with its own `RP-Version`, and the client
/// retries with that.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_array.hpp"
#include "libchiaking/detail/wire_sizes.hpp"
#include "src/core/net/protocol/remote_play/registered_console.hpp"

#include "limits.hpp"
#include "request_head.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"
#include "src/core/net/transport/http/status.hpp"

namespace libchiaking::net::protocol::remote_play {

/// @brief Why a session request could not be encoded.
enum class RequestCodecError : std::uint8_t {
  /// @brief The target is RegistrationTarget::ps4_unknown or unknown.
  invalid_target,
  /// @brief The host is empty or holds characters outside visible ASCII.
  invalid_host,
  /// @brief The port is 0.
  invalid_port,
  /// @brief The registration key is not exactly 16 bytes.
  invalid_key,
  /// @brief The head did not fit in rp_max_header_bytes.
  overflow,
};

/// @brief An encoded session request head, ready to send.
///
/// Holds the registration key in hex, so it lives in wiped storage and is
/// move-only. Made only by SessionCodec::encode_request.
class SessionRequest {
 public:
  /// @brief The bounded head the request is written into.
  using Head = RequestHead<rp_max_header_bytes>;

  /// @brief The request as text.
  ///
  /// @return A view into this request.
  std::string_view view() const noexcept { return head_.view(); }
  /// @brief The request as bytes to send.
  ///
  /// @return A view into this request.
  std::span<const std::byte> bytes() const noexcept { return head_.bytes(); }

 private:
  friend class SessionCodec;

  /// @brief The request head.
  Head head_;
};

/// @brief Why a session reply could not be read.
enum class ResponseCodecError : std::uint8_t {
  /// @brief The reply head has not fully arrived; read more.
  incomplete,
  /// @brief The reply head is longer than rp_max_header_bytes, or that many
  /// bytes arrived without completing it.
  too_large,
  /// @brief The bytes are not an HTTP/1.1 reply head.
  invalid,
  /// @brief The reply was 200 but `RP-Nonce` is not 16 bytes of standard
  /// base64.
  invalid_nonce,
  /// @brief The requested target is not one with an RP version.
  invalid_target,
};

/// @brief The console's acceptance of a session request.
///
/// Holds the nonce, a secret, so it is move-only and wiped on destruction.
class SessionGrant {
 public:
  /// @brief Makes a grant from the decoded nonce.
  /// @param nonce The decoded 16-byte `RP-Nonce`.
  /// @param header_size How many bytes the reply head took.
  SessionGrant(SecretArray<rp_nonce_size> nonce,
               std::size_t header_size) noexcept
      : nonce_(std::move(nonce)), header_size_(header_size) {}
  SessionGrant(const SessionGrant&) = delete;
  SessionGrant& operator=(const SessionGrant&) = delete;
  /// @brief Takes over another grant.
  SessionGrant(SessionGrant&&) noexcept = default;
  /// @brief Takes over another grant, wiping the nonce this held.
  SessionGrant& operator=(SessionGrant&&) noexcept = default;
  ~SessionGrant() = default;

  /// @brief The decoded 16-byte `RP-Nonce`.
  /// @return The nonce.
  auto nonce() const noexcept -> const SecretArray<rp_nonce_size>& {
    return nonce_;
  }
  /// @brief Hands the nonce over, leaving this grant's copy wiped.
  /// @return The nonce.
  auto take_nonce() noexcept -> SecretArray<rp_nonce_size> {
    return std::move(nonce_);
  }
  /// @brief How many bytes the reply head took.
  /// @return The head's length.
  auto header_size() const noexcept -> std::size_t { return header_size_; }

 private:
  /// @brief The nonce.
  SecretArray<rp_nonce_size> nonce_;
  /// @brief The head's length.
  std::size_t header_size_;
};

/// @brief The console's refusal of a session request.
class SessionRefusal {
 public:
  /// @brief How the console refused.
  enum class Kind : std::uint8_t {
    /// @brief Any refusal other than a version mismatch, such as a console in
    /// use or an unknown account; see application_reason().
    rejected = 1,
    /// @brief The console wants a different RP version. When it named one the
    /// client knows, retry_target() says which.
    version_mismatch = 2,
  };

  /// @brief Makes a rejection.
  /// @param status The HTTP status code as sent.
  /// @param application_reason The `RP-Application-Reason`, or 0.
  /// @return The refusal.
  static constexpr auto rejected(std::int32_t status,
                                 std::uint32_t application_reason) noexcept
      -> SessionRefusal {
    return {Kind::rejected, status, application_reason, std::nullopt};
  }
  /// @brief Makes a version mismatch.
  /// @param status The HTTP status code as sent.
  /// @param application_reason The `RP-Application-Reason`, or 0.
  /// @param retry_target The known variant the console asked for, if any.
  /// @return The refusal.
  static constexpr auto version_mismatch(
      std::int32_t status, std::uint32_t application_reason,
      std::optional<RegistrationTarget> retry_target) noexcept
      -> SessionRefusal {
    return {Kind::version_mismatch, status, application_reason, retry_target};
  }

  /// @brief How the console refused.
  /// @return The kind.
  constexpr auto kind() const noexcept -> Kind { return kind_; }
  /// @brief The HTTP status code as sent, which may lie outside the HTTP range.
  /// @return The code.
  constexpr auto status() const noexcept -> std::int32_t { return status_; }
  /// @brief The status code as an HTTP status.
  /// @return The code when it lies in the HTTP range, else 0.
  constexpr auto http_status() const noexcept -> std::uint16_t {
    return status_ > 0 && status_ <= net::HttpStatus::largest_code
               ? static_cast<std::uint16_t>(status_)
               : std::uint16_t{0};
  }
  /// @brief The `RP-Application-Reason` value, parsed as hexadecimal. See
  /// rp_reason.hpp.
  /// @return The reason, or 0 when absent or unreadable.
  constexpr auto application_reason() const noexcept -> std::uint32_t {
    return application_reason_;
  }
  /// @brief The variant to retry with, when the console asked for a different
  /// RP version that the client knows.
  /// @return The variant, or nothing.
  constexpr auto retry_target() const noexcept
      -> std::optional<RegistrationTarget> {
    return retry_target_;
  }
  /// @brief This refusal as it stands after the last allowed retry: no retry
  /// target is offered, and a mismatch the console did not state with
  /// rp_reason_rp_version becomes a rejection.
  /// @return The settled refusal.
  constexpr auto settled() const noexcept -> SessionRefusal {
    if (kind_ == Kind::version_mismatch &&
        application_reason_ == rp_reason_rp_version)
      return version_mismatch(status_, application_reason_, std::nullopt);
    if (kind_ == Kind::version_mismatch)
      return rejected(status_, application_reason_);
    return *this;
  }

 private:
  /// @brief Makes a refusal; see rejected() and version_mismatch().
  /// @param kind How the console refused.
  /// @param status The HTTP status code as sent.
  /// @param application_reason The `RP-Application-Reason`, or 0.
  /// @param retry_target The variant to retry with, if any.
  constexpr SessionRefusal(Kind kind, std::int32_t status,
                           std::uint32_t application_reason,
                           std::optional<RegistrationTarget> retry_target)
      : kind_(kind),
        status_(status),
        application_reason_(application_reason),
        retry_target_(retry_target) {}

  /// @brief How the console refused.
  Kind kind_;
  /// @brief The status as sent.
  std::int32_t status_;
  /// @brief The application reason.
  std::uint32_t application_reason_;
  /// @brief The variant to retry with.
  std::optional<RegistrationTarget> retry_target_;
};

/// @brief The console's answer to a session request: a grant, or the refusal
/// it sent instead.
using SessionResponse = Expected<SessionGrant, SessionRefusal>;

/// @brief Encodes session requests and classifies the console's replies.
///
/// Stateless; every member is static and may be called from any thread.
class SessionCodec {
 public:
  /// @brief Encodes the session request head for a registered console.
  ///
  /// Writes `GET <path> HTTP/1.1`, `Host: <host>:<port>`,
  /// `User-Agent: remoteplay Windows`, `Connection: close`,
  /// `Content-Length: 0`, `RP-Registkey: <hex>` and `Rp-Version: <version>`.
  /// The registration key is hex-encoded up to its first NUL byte, since it is
  /// stored as NUL-padded text.
  ///
  /// @param target The console variant, which picks the path and version.
  /// @param host The console's host or address as it goes in `Host`.
  /// @param port The console's port, normally 9295.
  /// @param registration_key The 16-byte registration key from pairing.
  /// @return The request, or why it could not be made.
  /// @retval RequestCodecError::invalid_target The target has no RP version.
  /// @retval RequestCodecError::invalid_host The host is unusable.
  /// @retval RequestCodecError::invalid_port The port is 0.
  /// @retval RequestCodecError::invalid_key The key is not 16 bytes.
  /// @retval RequestCodecError::overflow The head did not fit.
  [[nodiscard]] static auto encode_request(
      RegistrationTarget target, std::string_view host, std::uint16_t port,
      std::span<const std::byte> registration_key)
      -> Expected<SessionRequest, RequestCodecError>;

  /// @brief Reads and classifies the console's reply to a session request.
  ///
  /// A 200 with `RP-Nonce` is accepted. An `RP-Application-Reason` of
  /// rp_reason_rp_version, or rp_reason_unknown with an `RP-Version` that
  /// differs from the one requested, is a version mismatch; the retry target is
  /// the stated version, read in the requested console's family (with "5.0"
  /// accepted as PS4 RP 9.0). Anything else is rejected. Only the first copy of
  /// each header counts.
  ///
  /// @param requested The variant the request was sent for.
  /// @param received The bytes received so far.
  /// @return The classified reply, or why it cannot be read yet.
  /// @retval ResponseCodecError::invalid_target The requested target has no RP
  ///     version.
  /// @retval ResponseCodecError::incomplete The head has not fully arrived.
  /// @retval ResponseCodecError::too_large The head is too long.
  /// @retval ResponseCodecError::invalid The bytes are not an HTTP head.
  /// @retval ResponseCodecError::invalid_nonce The nonce is not 16 bytes of
  ///     base64.
  [[nodiscard]] static auto parse_response(
      RegistrationTarget requested,
      std::span<const std::byte> received) noexcept
      -> Expected<SessionResponse, ResponseCodecError>;

 private:
  /// @brief The request path and RP version text of a target.
  struct TargetFacts;
  /// @brief Looks up the path and version of a target.
  ///
  /// @param target The console variant.
  /// @return The facts, or RequestCodecError::invalid_target.
  static auto facts(RegistrationTarget target)
      -> Expected<TargetFacts, RequestCodecError>;
  /// @brief Reads the version a console asked for as a target in the requested
  /// console's family.
  ///
  /// @param requested The variant the request was sent for.
  /// @param version The console's `RP-Version` value.
  /// @return The target to retry with, or nothing for an unknown version.
  static std::optional<RegistrationTarget> retry_target(
      RegistrationTarget requested, std::string_view version) noexcept;
  /// @brief Decodes the base64 `RP-Nonce` value.
  ///
  /// @param text The header value.
  /// @return The 16 bytes, or nothing when it did not decode to exactly 16.
  static auto decode_nonce(std::string_view text) noexcept
      -> std::optional<SecretArray<rp_nonce_size>>;
};

}  // namespace libchiaking::net::protocol::remote_play
