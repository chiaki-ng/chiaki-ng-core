/// @file
/// @brief Declares GameMedia, the PSN game media service's cloud media
/// gallery, read with the app token to download what a PS5 cloud console
/// captured.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/auth/credentials.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace libchiaking::psn::rest {

/// @brief Where one capture in the cloud media gallery can be downloaded.
/// Plain data; text fields are empty when PS omitted them.
struct GalleryCapture {
  /// @brief The address of the full file, which carries its own access
  /// token in its query.
  std::string download_url;
  /// @brief The `fileType`, such as `jpg` or `mp4`.
  std::string file_type;
  /// @brief The `fileSize` in bytes; 0 when not given.
  std::uint64_t file_size{};
};

/// @brief Reads the cloud media gallery with the app token.
///
/// Holds a reference to the HTTP client, which must outlive it, and the token.
/// Each call blocks for one round trip. Not for concurrent use.
class GameMedia {
 public:
  /// @brief The failure of download_address().
  using DownloadAddressError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief The failure of capture().
  using CaptureError = Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;
  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  /// @param token The app token.
  GameMedia(net::http::Client& client, auth::BearerToken token) noexcept;

  /// @brief Finds where one capture can be downloaded.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/gameMediaService/v2/c2s/category/cloudMediaGallery/ugcType/all?ugcIds=<id>&includeTokenizedUrls=true`.
  ///
  /// @param gallery_id The capture's id in the gallery, as a cloud console
  ///     reports it once its upload finished.
  /// @return Where to download it.
  /// @retval bad_reply::InvalidSchema The reply has no `ugcDocument`
  ///     naming the capture with a download address.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto capture(std::string_view gallery_id)
      -> Expected<GalleryCapture, CaptureError>;
  /// @brief Asks for a download address of one capture that carries its own
  /// access, as the gallery's own addresses need the account's token.
  ///
  /// GET
  /// `https://m.np.playstation.com/api/gameMediaService/v2/c2s/ugc/<id>/url`.
  ///
  /// @param gallery_id The capture's id in the gallery.
  /// @return The signed address of the full file: `downloadUrl`, else
  ///     `videoUrl`, else `screenshotUrl`.
  /// @retval bad_reply::InvalidSchema The reply holds none of them.
  /// @retval Error A transport failure, an error status or an unreadable
  ///     reply.
  [[nodiscard]] auto download_address(std::string_view gallery_id)
      -> Expected<std::string, DownloadAddressError>;

 private:
  /// @brief The gallery's documents of every capture type.
  static constexpr std::string_view gallery_url =
      "https://m.np.playstation.com/api/gameMediaService/v2/c2s/category/"
      "cloudMediaGallery/ugcType/all";

  /// @brief The address of one capture's download addresses, before the id.
  static constexpr std::string_view ugc_url =
      "https://m.np.playstation.com/api/gameMediaService/v2/c2s/ugc/";

  /// @brief The download address request.
  static constexpr net::http::Endpoint address_endpoint{
      .label = "psn.game_media.download_address",
      .log_query = false,
  };

  /// @brief The request; the query is not logged.
  static constexpr net::http::Endpoint capture_endpoint{
      .label = "psn.game_media.capture",
      .log_query = false,
  };

  /// @brief The reply's `ugcDocument`.
  struct Reply;

  /// @brief The client the request goes through.
  net::http::Client& client_;
  /// @brief The app token.
  auth::BearerToken token_;
};

}  // namespace libchiaking::psn::rest
