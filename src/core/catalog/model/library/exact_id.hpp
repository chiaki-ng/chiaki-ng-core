/// @file
/// @brief Declares ExactId and its SkuId and TitleId instances, identifiers
/// that PS5 cloud streaming matches character for character.
///
/// PS5 identity is exact: an owned entitlement, a Premium SKU and an exclusion
/// rule are joined by the id PS sent, never by a title key or a name. This
/// header holds the identifier type for the ids that need no further reading.
#pragma once

#include <algorithm>
#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::catalog {

/// @brief An identifier PS sent, kept exactly as received and told apart
/// from other identifiers by its kind.
///
/// A value is always non-empty, at most ExactId::limit bytes long and made only
/// of printable ASCII without spaces; parse is the only way to build one. The
/// text is never normalised, so two ids are equal only when PS wrote them
/// identically. A value is immutable and owns its text; copies may be shared
/// between threads freely.
///
/// @tparam Kind An incomplete tag type that makes each instance a distinct
///     type, so a SkuId cannot be passed where a TitleId is expected. It is
///     never instantiated.
template <class Kind>
class ExactId {
 public:
  /// @brief The longest identifier accepted, in bytes.
  static constexpr std::size_t limit = 128;

  /// @brief Checks text PS sent and keeps it as an identifier.
  ///
  /// Does not block and allocates only the copy of the text.
  ///
  /// @param text The identifier as it appeared in PS's reply.
  /// @return The identifier, or std::nullopt when the text is empty, longer
  ///     than ExactId::limit bytes, or holds a space, a control character or
  ///     a byte outside printable ASCII.
  [[nodiscard]] static auto parse(std::string_view text)
      -> std::optional<ExactId> {
    if (text.empty() || text.size() > limit ||
        !std::ranges::all_of(text, printable))
      return std::nullopt;
    return ExactId{std::string{text}};
  }

  /// @brief The identifier as PS wrote it.
  ///
  /// @return The text, valid while this identifier lives; never empty.
  auto text() const noexcept -> const std::string& { return text_; }

  /// @brief Compares two identifiers byte for byte.
  friend auto operator==(const ExactId&, const ExactId&) -> bool = default;
  /// @brief Orders identifiers by their bytes, so they can key sorted
  /// containers.
  friend auto operator<=>(const ExactId&, const ExactId&)
      -> std::strong_ordering = default;

 private:
  /// @brief Keeps text that parse has already checked.
  ///
  /// @param text The checked identifier.
  explicit ExactId(std::string text) noexcept : text_(std::move(text)) {}

  /// @brief Whether a byte may appear in an identifier.
  ///
  /// @param value One byte of the candidate text.
  /// @return True for printable ASCII from '!' to '~'; false for a space,
  ///     control characters and bytes above 0x7e.
  static constexpr auto printable(char value) noexcept -> bool {
    return value > ' ' && value <= '~';
  }

  /// @brief The identifier exactly as received; set once at construction.
  std::string text_;
};

/// @brief A store SKU id, the purchasable variant of a product that an
/// entitlement or a Premium offer names.
using SkuId = ExactId<struct SkuKind>;
/// @brief A PS5 title id such as PPSA01234_00, as an entitlement's titleMeta
/// and the exclusion list carry it.
using TitleId = ExactId<struct TitleKind>;

}  // namespace libchiaking::catalog
