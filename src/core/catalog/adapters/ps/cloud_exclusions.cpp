#include "src/core/catalog/adapters/ps/cloud_exclusions.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/psn/rest/feature/feature_config.hpp"

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::ps {

CloudExclusions::CloudExclusions(net::http::Client& http,
                                 const platform::ClockProvider& wall,
                                 std::size_t maximum_response_bytes) noexcept
    : http_(http),
      stamp_(wall),
      maximum_response_bytes_(maximum_response_bytes) {}

auto CloudExclusions::fetch()
    -> Expected<cloud_stream::ExclusionList, Unanswered> {
  const auto listed =
      psn::rest::FeatureConfig{http_.limited(maximum_response_bytes_)}
          .excluded_title_ids();
  if (!listed) {
    http_.log(platform::LogSeverity::warning,
              "[CLOUD_STREAM] the restriction list could not be loaded");
    return unexpected(unanswered(listed.error()));
  }
  std::vector<TitleId> excluded;
  excluded.reserve(listed.value().size());
  for (const auto& text : listed.value()) {
    auto id = TitleId::parse(text);
    if (!id) {
      http_.log(platform::LogSeverity::warning,
                "[CLOUD_STREAM] the restriction list holds an entry "
                "that is not "
                "a title id");
      return unexpected(Unanswered{Unanswered::Reason::malformed,
                                   net::HttpStatus::ok.code()});
    }
    excluded.push_back(std::move(*id));
  }
  http_.log(platform::LogSeverity::info, "[CLOUD_STREAM] restriction list: " +
                                             std::to_string(excluded.size()) +
                                             " excluded");
  return cloud_stream::ExclusionList{std::move(excluded), stamp_.now()};
}

auto CloudExclusions::unanswered(
    const psn::rest::FeatureConfig::ExcludedTitleIdsError& error) noexcept
    -> Unanswered {
  using Reason = Unanswered::Reason;
  const auto status = error.http_status();
  switch (error.cause()) {
    case psn::rest::Cause::failure:
      if (error.is_cancelled()) return Unanswered::cancelled();
      return Unanswered{Reason::unavailable, status};
    case psn::rest::Cause::status:
      return Unanswered{status >= 500 ? Reason::unavailable : Reason::rejected,
                        status};
    case psn::rest::Cause::request:
      return Unanswered{Reason::rejected, status};
    case psn::rest::Cause::response:
      return Unanswered{Reason::malformed, status};
  }
  return Unanswered{Reason::unavailable, status};
}

}  // namespace libchiaking::catalog::ps
