#include "src/core/psn/rest/feature/feature_config.hpp"

#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <optional>
#include <utility>

namespace libchiaking::psn::rest {

struct FeatureConfig::Reply {
  using keys = json::ps::CamelCaseKeys;
  std::optional<std::vector<std::string>> excluded_title_ids;
};

FeatureConfig::FeatureConfig(net::http::Client client) noexcept
    : client_(std::move(client)) {}

auto FeatureConfig::excluded_title_ids()
    -> Expected<std::vector<std::string>, ExcludedTitleIdsError> {
  net::http::Request request{endpoint, url};
  request.header("Accept", "application/json");
  auto reply = ExcludedTitleIdsError::checked_reply<Reply>(
      client_.send(std::move(request)), {});
  if (!reply) return unexpected(std::move(reply.error()));
  if (!reply.value().excluded_title_ids)
    return unexpected(ExcludedTitleIdsError{bad_reply::InvalidSchema{}});
  return std::move(*reply.value().excluded_title_ids);
}

}  // namespace libchiaking::psn::rest
