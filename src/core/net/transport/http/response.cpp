#include "src/core/net/transport/http/response.hpp"

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::http {

Response::Response(platform::HttpResponse response, int attempts,
                   std::size_t maximum_json_bytes) noexcept
    : response_(std::move(response)),
      attempts_(attempts),
      maximum_json_bytes_(maximum_json_bytes) {}

auto Response::header(std::string_view name) const noexcept
    -> std::optional<std::string_view> {
  const auto found = std::ranges::find_if(
      response_.headers, [name](const platform::HttpHeader& header) {
        return text::ieq(header.name, name);
      });
  if (found == response_.headers.end()) return std::nullopt;
  return found->value.view();
}

}  // namespace libchiaking::net::http
