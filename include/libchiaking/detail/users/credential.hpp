/// @file
/// @brief Declares Credential, the wiping holder every PSN token and cookie is
/// carried in.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/platform/http/values.hpp"

#include <libchiaking/detail/expected.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::users {

/// @brief Where a kind of credential goes in an HTTP request.
struct CredentialPlacement {
  /// @brief The name of the header that carries the credential.
  std::string_view header;
  /// @brief The text placed before the secret in the header's value.
  std::string_view prefix;
};

/// @brief A secret credential of one particular kind, such as an NPSSO or an
/// app token.
///
/// The secret lives in a SecretString, which wipes its memory when destroyed,
/// from the moment the credential is created. A credential is move-only; the
/// only way to duplicate the secret is clone(). The `Kind` parameter makes each
/// sort of credential a distinct type, so an app token cannot be passed where a
/// Remote Play token is expected.
///
/// @tparam Kind A tag type with a static `label`. Kinds that can be sent as a
///     request header also provide a static `placement`; see HeaderCredential.
///     Kinds with a length limit provide a static `maximum_bytes`, which
///     enables refusal_of(), admits(), admit() and refusal().
template <class Kind>
class Credential {
 public:
  /// @brief The tag type that says which kind of credential this is.
  using kind = Kind;

  /// @brief Adopts a secret that is already in wiping storage.
  ///
  /// @param value The secret.
  /// @param label Name for diagnostics. It is not copied, so it must outlive
  ///     the credential; pass a string literal.
  explicit Credential(SecretString value,
                      std::string_view label = Kind::label) noexcept
      : value_(std::move(value)), label_(label) {}

  /// @brief Copies a secret from plain text into wiping storage.
  ///
  /// The caller remains responsible for the memory `value` points at, which
  /// this does not wipe.
  ///
  /// @param value The secret text.
  /// @param label Name for diagnostics. It is not copied, so it must outlive
  ///     the credential; pass a string literal.
  explicit Credential(std::string_view value,
                      std::string_view label = Kind::label)
      : value_(value), label_(label) {}

  /// @brief Makes a deliberate second copy of the credential, which wipes
  /// itself independently.
  ///
  /// Never blocks. Safe on any thread that may read this credential.
  ///
  /// @return The copy, with the same secret and label. It cannot fail.
  auto clone() const -> Credential {
    return Credential{value_.clone(), label_};
  }

  /// @brief A short name for the credential, used in diagnostics in place of
  /// the secret.
  ///
  /// @return The label given at construction; it holds nothing secret.
  auto label() const noexcept -> std::string_view { return label_; }
  /// @brief The secret in its wiping holder.
  ///
  /// @return A reference valid while the credential lives and is not moved
  ///     from. Copy it only with SecretString::clone.
  auto secret() const noexcept -> const SecretString& { return value_; }
  /// @brief The secret as text.
  ///
  /// @return A view valid while the credential lives and is not moved from;
  ///     do not copy it into a plain string.
  auto view() const noexcept -> std::string_view { return value_.view(); }
  /// @brief Whether the credential holds no secret at all.
  ///
  /// @return True when the secret is empty.
  auto empty() const noexcept -> bool { return value_.empty(); }

  /// @brief Why text is not acceptable as the secret of a credential of a
  /// kind that declares `maximum_bytes`.
  enum class Refusal : std::uint8_t {
    /// @brief The text is empty.
    empty,
    /// @brief The text is longer than `Kind::maximum_bytes`.
    too_long,
    /// @brief The text holds a byte other than visible ASCII, or a double
    /// quote, comma, semicolon or backslash, so it cannot be sent unchanged
    /// as a header or cookie value.
    unsendable,
  };

  /// @brief Tells why text is not acceptable as the secret of a credential
  /// of this kind.
  ///
  /// Never blocks and touches no shared state, so it is safe on any thread.
  ///
  /// @param text The candidate secret. It is only read, never kept.
  /// @return Nothing when the text is acceptable.
  /// @retval Refusal::empty The text is empty.
  /// @retval Refusal::too_long The text is longer than `Kind::maximum_bytes`.
  /// @retval Refusal::unsendable The text holds a byte that cannot be sent
  ///     unchanged.
  static constexpr auto refusal_of(std::string_view text) noexcept
      -> std::optional<Refusal>
    requires requires { Kind::maximum_bytes; }
  {
    if (text.empty()) return Refusal::empty;
    if (text.size() > Kind::maximum_bytes) return Refusal::too_long;
    if (!std::ranges::all_of(text, sendable)) return Refusal::unsendable;
    return std::nullopt;
  }

  /// @brief Tells whether text is acceptable as the secret of a credential
  /// of this kind.
  ///
  /// Never blocks and touches no shared state, so it is safe on any thread.
  ///
  /// @param text The candidate secret. It is only read, never kept.
  /// @return True when refusal_of() finds nothing wrong.
  static constexpr auto admits(std::string_view text) noexcept -> bool
    requires requires { Kind::maximum_bytes; }
  {
    return !refusal_of(text);
  }

  /// @brief Makes a credential from a secret only if the secret is
  /// acceptable.
  ///
  /// Never blocks. On a refusal the secret is wiped with the argument.
  ///
  /// @param value The secret.
  /// @param label Name for diagnostics. It is not copied, so it must outlive
  ///     the credential; pass a string literal.
  /// @return The credential.
  /// @retval Refusal The reason refusal_of() gives.
  [[nodiscard]] static auto admit(SecretString value,
                                  std::string_view label = Kind::label)
      -> Expected<Credential, Refusal>
    requires requires { Kind::maximum_bytes; }
  {
    if (const auto refused = refusal_of(value.view()))
      return unexpected(*refused);
    return Credential{std::move(value), label};
  }

  /// @brief Tells why this credential's secret is not acceptable.
  ///
  /// Never blocks. Safe on any thread that may read the credential.
  ///
  /// @return Nothing when the secret is acceptable.
  /// @retval Refusal The reason refusal_of() gives.
  auto refusal() const noexcept -> std::optional<Refusal>
    requires requires { Kind::maximum_bytes; }
  {
    return refusal_of(view());
  }

 private:
  static constexpr auto sendable(char value) noexcept -> bool {
    return value >= '!' && value <= '~' && value != '"' && value != ',' &&
           value != ';' && value != '\\';
  }

  SecretString value_;
  std::string_view label_;
};

}  // namespace libchiaking::users
