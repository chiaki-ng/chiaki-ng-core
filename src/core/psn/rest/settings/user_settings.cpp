#include "src/core/psn/rest/settings/user_settings.hpp"

#include "src/core/net/transport/http/url.hpp"
#include "src/core/psn/rest/error_reply.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::psn::rest::user_settings_wire {

struct Setting {
  json::ps::Tolerant<std::string> value;
  json::ps::Tolerant<std::string> default_value;
};

}  // namespace libchiaking::psn::rest::user_settings_wire

template <>
struct glz::meta<libchiaking::psn::rest::user_settings_wire::Setting> {
  using T = libchiaking::psn::rest::user_settings_wire::Setting;
  static constexpr auto value =
      glz::object("value", &T::value, "default", &T::default_value);
};

namespace libchiaking::psn::rest {

UserSettings::UserSettings(net::http::Client& client,
                           auth::BearerToken token) noexcept
    : client_(client), token_(std::move(token)) {}

auto UserSettings::switched_on(std::string_view name)
    -> Expected<bool, SwitchedOnError> {
  net::http::Query url{settings_url};
  url.add("settings", name);
  net::http::Request request{setting_endpoint, std::move(url)};
  request.authorize(token_).header("Accept", "application/json");
  auto reply =
      SwitchedOnError::checked_reply<std::vector<user_settings_wire::Setting>>(
          client_.send(std::move(request)), token_.view());
  if (!reply) return unexpected(std::move(reply.error()));
  if (reply->empty())
    return unexpected(SwitchedOnError{bad_reply::InvalidSchema{}});
  auto& setting = reply->front();
  if (setting.value && !setting.value->empty()) return *setting.value == "on";
  if (setting.default_value && !setting.default_value->empty())
    return *setting.default_value == "on";
  return unexpected(SwitchedOnError{bad_reply::InvalidSchema{}});
}

}  // namespace libchiaking::psn::rest
