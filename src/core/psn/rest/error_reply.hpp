/// @file
/// @brief Defines the members of rest::Error that decode a reply, which need
/// the JSON parser that the rest of Error's users do not.
#pragma once

#include "src/core/net/transport/http/response_document.hpp"
#include "src/core/psn/rest/error.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <string_view>
#include <utility>

namespace libchiaking::psn::rest {

template <ReasonTag... Reasons>
template <class Reply>
auto Error<Reasons...>::reply_of(const net::http::Response& response)
    -> Expected<Reply, Error>
  requires OneOf<bad_reply::InvalidJson, Reasons...> &&
           OneOf<bad_reply::InvalidSchema, Reasons...>
{
  auto reply = json::ps::read_reply<Reply>(response.body());
  if (reply) return std::move(reply.value());
  if (json::well_formed(response.body()))
    return unexpected(Error{bad_reply::InvalidSchema{}});
  return unexpected(Error{bad_reply::InvalidJson{}});
}

template <ReasonTag... Reasons>
template <class Reply>
auto Error<Reasons...>::checked_reply(const net::http::Response& response,
                                      std::string_view redact)
    -> Expected<Reply, Error> {
  if (auto refused = refusal(response, redact))
    return unexpected(std::move(*refused));
  return reply_of<Reply>(response);
}

template <ReasonTag... Reasons>
template <class Reply>
auto Error<Reasons...>::checked_reply(
    Expected<net::http::Response, net::http::Failure> sent,
    std::string_view redact) -> Expected<Reply, Error> {
  if (!sent) return unexpected(Error{sent.error()});
  return checked_reply<Reply>(sent.value(), redact);
}

}  // namespace libchiaking::psn::rest
