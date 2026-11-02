/// @file
/// @brief Declares FeatureConfig, a public configuration document, read for
/// the PS5 games PS excludes from cloud streaming.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/error.hpp"

#include <libchiaking/detail/expected.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::psn::rest {

/// @brief Reads the configuration document anonymously.
///
/// Owns a copy of the HTTP client (whose provider must outlive it). Each call
/// blocks for one round trip. Not for concurrent use.
class FeatureConfig {
 public:
  /// @brief The request's diagnostic label.
  static constexpr std::string_view label = "psn.feature.config";

  /// @brief Prepares the request.
  ///
  /// @param client The client the request goes through.
  explicit FeatureConfig(net::http::Client client) noexcept;

  /// @brief The failure of excluded_title_ids().
  using ExcludedTitleIdsError =
      Error<bad_reply::InvalidJson, bad_reply::InvalidSchema>;

  /// @brief Reads the titles excluded from cloud streaming.
  ///
  /// GET `https://feature.api.playstation.com/portal/np/portal-config.json`
  /// with no credential; `excludedTitleIds` is read. Each entry is matched
  /// against a title id exactly, or against a product id as a substring, by
  /// the catalogue.
  ///
  /// @return The excluded ids, in reply order.
  /// @retval bad_reply::InvalidSchema The document has no
  ///     `excludedTitleIds`, or has it in another shape.
  /// @retval bad_reply::InvalidJson The document is not JSON.
  /// @retval ExcludedTitleIdsError A transport failure or an error status.
  [[nodiscard]] auto excluded_title_ids()
      -> Expected<std::vector<std::string>, ExcludedTitleIdsError>;

 private:
  /// @brief The document's `excludedTitleIds`.
  struct Reply;

  /// @brief The configuration document.
  static constexpr std::string_view url =
      "https://feature.api.playstation.com/portal/np/portal-config.json";
  /// @brief The request.
  static constexpr net::http::Endpoint endpoint{.label = label};

  /// @brief The client the request goes through.
  net::http::Client client_;
};

}  // namespace libchiaking::psn::rest
