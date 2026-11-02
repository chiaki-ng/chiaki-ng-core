/// @file
/// @brief Declares MediaList, a game's store media as GraphQL lists them, and
/// TitleMedia, the pieces the catalogue shows chosen from it by role.
#pragma once

#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/glaze.hpp>

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::graphql {

/// @brief One store media entry. Plain data.
struct StoreMedia {
  /// @brief The `role`, such as `MASTER`, `LOGO` or `SCREENSHOT`; never
  /// empty.
  std::string role;
  /// @brief The `type`, `IMAGE` or `VIDEO`.
  std::string type;
  /// @brief The `url`; never empty.
  std::string url;
};

/// @brief The media a game is shown with. Plain data; empty when none fits.
struct TitleMedia {
  /// @brief The first `MASTER` image, else `EDITION_KEY_ART`, else
  /// `BACKGROUND_LAYER_ART`.
  std::string key_art;
  /// @brief The first `LOGO` image.
  std::string logo;
  /// @brief The first `GAMEHUB_COVER_ART` image, else `PORTRAIT_BANNER`.
  std::string cover;
  /// @brief The first `PREVIEW` video.
  std::string preview_video;
  /// @brief The first ten `SCREENSHOT` images, in reply order.
  std::vector<std::string> screenshots;
};

/// @brief A game's store media, entries without a role or URL dropped.
class MediaList {
 public:
  /// @brief A media entry as sent.
  struct Entry {
    /// @brief The role.
    json::ps::Tolerant<std::string> role;
    /// @brief The type.
    json::ps::Tolerant<std::string> type;
    /// @brief The URL.
    json::ps::Tolerant<std::string> url;
  };
  /// @brief The list as decoded, each entry and the list itself tolerated.
  using Listed = json::ps::Tolerant<std::vector<json::ps::Tolerant<Entry>>>;

  /// @brief Makes an empty list.
  MediaList() = default;
  /// @brief Keeps the entries with a role and a URL.
  ///
  /// @param listed The decoded list; empty when it was not a list.
  explicit MediaList(Listed listed);

  /// @brief Reads the entries.
  ///
  /// @return The entries, in reply order; valid while this object lives.
  auto entries() const& noexcept -> std::span<const StoreMedia> {
    return entries_;
  }
  /// @brief Gives up the entries.
  ///
  /// @return The entries, moved out.
  auto entries() && noexcept -> std::vector<StoreMedia> {
    return std::move(entries_);
  }
  /// @brief Chooses the media a game is shown with, by role.
  ///
  /// @return The choice TitleMedia describes; a role compared exactly, images
  ///     only for the still pieces.
  auto title_media() const -> TitleMedia;

 private:
  /// @brief The most screenshots kept.
  static constexpr std::size_t maximum_screenshots = 10;

  /// @brief The entries.
  std::vector<StoreMedia> entries_;
};

}  // namespace libchiaking::psn::graphql

/// @brief Lets glaze read a MediaList from any JSON value.
template <>
struct glz::from<glz::JSON, libchiaking::psn::graphql::MediaList> {
  /// @brief Reads a MediaList.
  ///
  /// A value that is not a list reads as empty.
  ///
  /// @tparam Opts The glaze read options.
  /// @param value Where the result goes.
  /// @param ctx The glaze context; an error in it ends the read.
  /// @param it The read position, advanced past the value.
  /// @param end The end of the input.
  template <auto Opts>
  static void op(libchiaking::psn::graphql::MediaList& value,
                 glz::is_context auto&& ctx, auto&& it, auto end) {
    libchiaking::psn::graphql::MediaList::Listed listed{};
    glz::parse<glz::JSON>::op<Opts>(listed, ctx, it, end);
    if (bool(ctx.error)) return;
    value = libchiaking::psn::graphql::MediaList{std::move(listed)};
  }
};
