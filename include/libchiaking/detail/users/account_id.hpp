/// @file
/// @brief Declares AccountId, the number that identifies a PSN account.
#pragma once

#include "libchiaking/detail/wire_sizes.hpp"

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>

namespace libchiaking {

/// @brief Most decimal digits an account id can have: the length of the largest
/// 64-bit unsigned number.
inline constexpr std::size_t account_id_max_digits = 20;

/// @brief The 64-bit number PSN assigns to an account.
///
/// It is the stable identity of a user: pairings, saved sign-ins and consoles'
/// account lists are all keyed by it. It is not a secret. The value 0 does not
/// name an account, and SignedInUsers refuses to admit a user with it.
class AccountId {
 public:
  /// @brief Wraps an account number.
  ///
  /// @param value The number PSN assigned.
  explicit constexpr AccountId(std::uint64_t value) noexcept : value_(value) {}

  /// @brief Reads an account id written in decimal, the form PSN's web APIs
  /// use.
  ///
  /// @param decimal One to account_id_max_digits ASCII digits, with no sign
  ///     or spaces.
  /// @return The id, or nothing when the text is empty, too long, holds
  ///     anything but digits, or does not fit in 64 bits.
  [[nodiscard]] static auto parse(std::string_view decimal) noexcept
      -> std::optional<AccountId> {
    if (decimal.empty() || decimal.size() > account_id_max_digits)
      return std::nullopt;
    std::uint64_t value{};
    const auto parsed =
        std::from_chars(decimal.data(), decimal.data() + decimal.size(), value);
    if (parsed.ec != std::errc{} ||
        parsed.ptr != decimal.data() + decimal.size())
      return std::nullopt;
    return AccountId{value};
  }

  /// @brief Longest Base64 text from_base64() will read, in bytes.
  static constexpr std::size_t base64_limit = 23;

  /// @brief Reads an account id from Base64 text that encodes the eight bytes
  /// of bytes().
  ///
  /// @param text The Base64 text.
  /// @return The id, or nothing when the text is empty, longer than
  ///     base64_limit, holds a NUL, or does not decode to exactly eight
  ///     bytes.
  [[nodiscard]] static auto from_base64(std::string_view text) noexcept
      -> std::optional<AccountId>;

  /// @brief Builds an account id from its eight-byte wire form, least
  /// significant byte first.
  ///
  /// @param bytes The eight bytes.
  /// @return The id.
  static constexpr auto from_bytes(
      std::span<const std::byte, psn_account_id_size> bytes) noexcept
      -> AccountId {
    std::uint64_t value{};
    for (std::size_t index = 0; index < bytes.size(); ++index)
      value |= std::to_integer<std::uint64_t>(bytes[index]) << (8U * index);
    return AccountId{value};
  }

  /// @brief The account number.
  ///
  /// @return The account number.
  constexpr auto value() const noexcept -> std::uint64_t { return value_; }

  /// @brief The id in its eight-byte wire form, least significant byte first,
  /// as the console protocol carries it.
  ///
  /// @return The eight bytes.
  constexpr auto bytes() const noexcept
      -> std::array<std::byte, psn_account_id_size> {
    std::array<std::byte, psn_account_id_size> result{};
    for (std::size_t index = 0; index < result.size(); ++index)
      result[index] = static_cast<std::byte>(value_ >> (8U * index));
    return result;
  }

  /// @brief The id written in decimal, the form PSN's web APIs use.
  ///
  /// @return The decimal digits, never empty.
  auto decimal() const -> std::string { return std::to_string(value_); }

  /// @brief Compares the numbers.
  friend constexpr auto operator==(const AccountId&, const AccountId&) noexcept
      -> bool = default;

 private:
  std::uint64_t value_;
};

}  // namespace libchiaking
