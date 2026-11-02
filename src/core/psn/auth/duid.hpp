/// @file
/// @brief Declares Duid, a checked GK device unique id ready to be put in
/// an authorisation request.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "src/core/psn/auth/gk_ids.hpp"
#include "src/core/utils/text/hex.hpp"

#include <algorithm>
#include <optional>
#include <string_view>
#include <utility>

namespace libchiaking::psn::auth {

/// @brief The device unique id the authorisation server binds GK codes to.
///
/// A value is always gk_duid_length characters: gk_duid_prefix then
/// lowercase hex digits. PS treats it as identifying, so it is held in wiping
/// memory. Move-only by way of its member; immutable once made.
class Duid {
 public:
  /// @brief Checks a DUID before it is sent.
  ///
  /// @param digits The DUID as text.
  /// @return The DUID, or nothing when it has the wrong length, does not
  ///     start with gk_duid_prefix, or has an upper-case or non-hex digit
  ///     after it.
  [[nodiscard]] static auto parse(std::string_view digits)
      -> std::optional<Duid> {
    if (digits.size() != gk_duid_length ||
        !digits.starts_with(gk_duid_prefix) ||
        !std::ranges::all_of(digits.substr(gk_duid_prefix.size()),
                             text::lower_hex_digit))
      return std::nullopt;
    return Duid{SecretString{digits}};
  }

  /// @brief Reads the DUID.
  ///
  /// @return The DUID text; valid while this object lives.
  auto view() const noexcept -> std::string_view { return text_.view(); }

 private:
  /// @brief Wraps text parse() has checked.
  ///
  /// @param text The checked DUID.
  explicit Duid(SecretString text) noexcept : text_(std::move(text)) {}

  /// @brief The DUID text, satisfying the class invariant.
  SecretString text_;
};

}  // namespace libchiaking::psn::auth
