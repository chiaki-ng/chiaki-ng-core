#include "src/core/net/transport/http/request.hpp"

#include <algorithm>
#include <utility>

namespace libchiaking::net::http {

Request::Request(const Endpoint& endpoint, SecretString url)
    : endpoint_(endpoint), url_(std::move(url)) {}

Request::Request(const Endpoint& endpoint, std::string_view url)
    : Request(endpoint, SecretString{url}) {}

Request::Request(const Endpoint& endpoint, Query url)
    : Request(endpoint, std::move(url).take()) {}

auto Request::header(std::string_view name, std::string_view value)
    -> Request& {
  return header(name, SecretString{value});
}

auto Request::header(std::string_view name, SecretString value) -> Request& {
  headers_.push_back({std::string{name}, std::move(value)});
  return *this;
}

auto Request::credential_label(std::string_view label) noexcept -> Request& {
  credential_ = label;
  return *this;
}

auto Request::body(SecretBytes body) noexcept -> Request& {
  body_ = std::move(body);
  return *this;
}

auto Request::body(std::string_view content_type, SecretBytes body)
    -> Request& {
  header(content_type_header, content_type);
  return this->body(std::move(body));
}

auto Request::json(std::string_view body) -> Request& {
  return this->body(json_content_type, SecretBytes{body});
}

auto Request::json(const SecretString& body) -> Request& {
  return this->body(json_content_type, SecretBytes{body.bytes()});
}

auto Request::form(Form form) -> Request& {
  const auto encoded = std::move(form).take();
  return body(form_content_type, SecretBytes{encoded.bytes()});
}

auto Request::release() && -> platform::HttpRequest {
  const auto total =
      std::max(endpoint_.total_timeout, std::chrono::milliseconds{1});
  platform::HttpRequest request;
  request.method = endpoint_.method;
  request.label = endpoint_.label;
  request.url = std::move(url_);
  request.headers = std::move(headers_);
  request.body = std::move(body_);
  request.total_timeout = total;
  request.connect_timeout = std::min(
      total, std::max(endpoint_.connect_timeout, std::chrono::milliseconds{1}));
  request.redirect_policy = platform::HttpRedirectPolicy::deny;
  request.maximum_response_bytes = endpoint_.maximum_response_bytes;
  request.provenance = {.credentials = credentials_,
                        .credential_label = credential_,
                        .carried = std::move(carried_),
                        .expected_statuses = endpoint_.expected_statuses};
  return request;
}

}  // namespace libchiaking::net::http
