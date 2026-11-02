#include "src/core/psn/rest/accounts/accounts_client.hpp"

#include "src/core/net/transport/http/status.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <array>
#include <type_traits>
#include <utility>

namespace libchiaking::psn::rest {

struct AccountsClient::AttributesBody {
  std::remove_const_t<decltype(validated_attributes)> attributes;
};

struct AccountsClient::Refusal {
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;
  struct Missing {
    Tolerant<std::string> name;
  };
  struct Validation {
    using keys = json::ps::CamelCaseKeys;
    Tolerant<std::vector<Tolerant<Missing>>> missing_elements;
  };
  struct Failure {
    using keys = json::ps::CamelCaseKeys;
    Tolerant<std::vector<Tolerant<Validation>>> validation_errors;
  };
  Tolerant<Failure> error;
};

auto AccountsClient::missing_elements(const net::http::Response& response)
    -> std::vector<std::string> {
  std::vector<std::string> names;
  auto refusal = json::ps::read_reply<Refusal>(response.body());
  if (!refusal || !refusal->error || !refusal->error->validation_errors)
    return names;
  for (auto& validation : *refusal->error->validation_errors) {
    if (!validation || !validation->missing_elements) continue;
    for (auto& missing : *validation->missing_elements)
      if (missing && missing->name && !missing->name->empty())
        names.push_back(*missing->name);
  }
  return names;
}

auto AccountsError::auth_refused() const noexcept -> bool {
  return !failure_ && net::HttpStatus{http_status_}.is_auth_refused();
}

AccountsClient::AccountsClient(net::http::Client client,
                               std::string_view user_agent) noexcept
    : client_(std::move(client)), user_agent_(user_agent) {}

auto AccountsClient::validate_attributes(const auth::BearerToken& bearer)
    -> Expected<AttributesAccepted, AccountsError> {
  const auto body = json::ps::write(AttributesBody{validated_attributes});
  if (!body)
    return unexpected(AccountsError{net::http::Failure::invalid_request()});
  net::http::Request request{validate_attributes_endpoint, attributes_url};
  request.authorize(bearer);
  if (!user_agent_.empty()) request.header("User-Agent", user_agent_);
  request.header("Accept", "application/json");
  request.json(body.value());
  auto sent = client_.send(std::move(request));
  if (!sent) return unexpected(AccountsError{sent.error()});
  const auto status = sent.value().status();
  if (net::HttpStatus{status} == net::HttpStatus::ok ||
      net::HttpStatus{status} == net::HttpStatus::no_content)
    return AttributesAccepted{status};
  return unexpected(AccountsError{status, missing_elements(sent.value())});
}

}  // namespace libchiaking::psn::rest
