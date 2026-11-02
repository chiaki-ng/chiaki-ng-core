/// @file
/// @brief Declares ContentUrl, a checked address on one of PS's content hosts.
#pragma once

#include <array>
#include <compare>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::catalog {

/// @brief An address guaranteed to point at one of PS's content hosts.
///
/// Only a checked address can be turned into a request or handed to a
/// frontend, so a URL taken from a store reply cannot send anyone to another
/// host. ImageRef and TrailerRef are the two kinds; they differ only in the
/// host they admit.
///
/// The value is immutable and valid from construction. It is safe to share
/// between threads.
///
/// @tparam Host A type with a static `origins`, the schemes and hosts an
///     address may start with, and a static `url_limit`, the longest address
///     accepted in bytes.
template <class Host>
class ContentUrl {
 public:
  /// @brief The schemes and hosts an address may start with.
  static constexpr auto origins = Host::origins;
  /// @brief Longest address accepted, in bytes.
  static constexpr std::size_t url_limit = Host::url_limit;

  /// @brief Checks an address and wraps it. Never blocks.
  ///
  /// @param url The address as the store gave it.
  /// @return The reference, or nothing unless the address starts with
  ///     one of origins, has a path, is at most url_limit bytes, uses only
  ///     letters, digits and the characters -._~/?=&%+ after the origin, and
  ///     has no
  ///     ".." in it.
  [[nodiscard]] static auto at(std::string_view url)
      -> std::optional<ContentUrl>;

  /// @brief The full address, unchanged from what was checked.
  ///
  /// @return The address text, valid while this object lives.
  auto url() const noexcept -> const std::string& { return url_; }

  /// @brief Compares the addresses byte for byte.
  friend auto operator==(const ContentUrl&, const ContentUrl&)
      -> bool = default;
  /// @brief Orders by address, so references can key a map or set.
  friend auto operator<=>(const ContentUrl&, const ContentUrl&)
      -> std::strong_ordering = default;

 private:
  explicit ContentUrl(std::string url) noexcept;

  static auto url_character(char value) noexcept -> bool;
  static auto origin_of(std::string_view url) noexcept
      -> std::optional<std::string_view>;

  std::string url_;
};

/// @brief PS's image hosts, the hosts ImageRef admits.
struct ImageHost {
  /// @brief The schemes and hosts an image address may start with.
  static constexpr std::array<std::string_view, 6> origins{
      "https://image.api.playstation.com/",
      "https://image.api.np.km.playstation.net/",
      "https://vulcan.dl.playstation.net/",
      "https://apollo2.dl.playstation.net/",
      "https://gs2-sec.ww.prod.dl.playstation.net/",
      "https://psnobj.prod.dl.playstation.net/",
  };
  /// @brief Longest address accepted, in bytes.
  static constexpr std::size_t url_limit = 512;
  /// @brief Largest image, in bytes, that is accepted and cached.
  static constexpr std::size_t largest_bytes = 16U * 1024U * 1024U;
};

/// @brief PS's video host, the one host TrailerRef admits.
struct TrailerHost {
  /// @brief The scheme and host every trailer address starts with.
  static constexpr std::array<std::string_view, 1> origins{
      "https://vulcan.dl.playstation.net/",
  };
  /// @brief Longest address accepted, in bytes.
  static constexpr std::size_t url_limit = 512;
};

/// @brief The address of a store image on PS's image host.
///
/// @see Catalog::image_of
using ImageRef = ContentUrl<ImageHost>;

/// @brief The address of a store trailer on PS's video host.
///
/// The catalog does not download trailers; a frontend plays the address itself.
///
/// @see GameDetails::trailers
using TrailerRef = ContentUrl<TrailerHost>;

extern template class ContentUrl<ImageHost>;
extern template class ContentUrl<TrailerHost>;

}  // namespace libchiaking::catalog
