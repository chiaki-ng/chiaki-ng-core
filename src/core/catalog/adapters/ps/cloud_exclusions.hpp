/// @file
/// @brief Declares ps::CloudExclusions, the adapter that reads the list
/// of titles excluded from cloud streaming.
///
/// It reads excludedTitleIds from
/// https://feature.api.playstation.com/portal/np/portal-config.json, asked
/// anonymously.
#pragma once

#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/cloud_stream/exclusions.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/psn/rest/feature/feature_config.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstddef>

namespace libchiaking::catalog::ps {

/// @brief Fetches one snapshot of the exclusion list.
///
/// Borrows the client and clock, which must outlive it.
class CloudExclusions {
 public:
  /// @brief Sets up the fetch.
  ///
  /// @param http Sends the request. Must outlive this object.
  /// @param wall Stamps the snapshot. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted.
  CloudExclusions(net::http::Client& http, const platform::ClockProvider& wall,
                  std::size_t maximum_response_bytes =
                      net::http::default_maximum_response_bytes) noexcept;

  /// @brief Reads the list now.
  ///
  /// Blocks on the reply; the request is bounded by the HTTP provider's
  /// timeouts and retries and abandoned when the client's stop token fires.
  ///
  /// @return The snapshot stamped now; Unanswered::Reason::malformed when an
  ///     entry is not a title id; otherwise the request's failure as
  ///     unanswered maps it.
  [[nodiscard]] auto fetch()
      -> Expected<cloud_stream::ExclusionList, Unanswered>;

 private:
  /// @brief Maps a request failure to a catalog failure.
  ///
  /// @param error The failure.
  /// @return A cancellation; Unanswered::Reason::unavailable for a transport
  ///     failure or a 5xx status; Unanswered::Reason::rejected for another
  ///     status; Unanswered::Reason::malformed for an unreadable reply.
  static auto unanswered(
      const psn::rest::FeatureConfig::ExcludedTitleIdsError& error) noexcept
      -> Unanswered;

  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps the snapshot.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
