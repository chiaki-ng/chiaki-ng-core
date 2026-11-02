#include "src/core/net/transport/http/url.hpp"

namespace libchiaking::net::http {

Query::Query(std::string_view base)
    : url_(base), has_query_(base.find('?') != std::string_view::npos) {}

auto Query::add(std::string_view name, std::string_view value, Escape escape)
    -> Query& {
  separate(name);
  PercentEncoding::append(url_, value, escape);
  return *this;
}

auto Query::add_verbatim(std::string_view name, std::string_view value)
    -> Query& {
  separate(name);
  url_.append(value);
  return *this;
}

void Query::separate(std::string_view name) {
  url_.push_back(has_query_ ? '&' : '?');
  has_query_ = true;
  url_.append(name);
  url_.push_back('=');
}

auto Form::add(std::string_view name, std::string_view value) -> Form& {
  if (!body_.empty()) body_.push_back('&');
  PercentEncoding::append(body_, name, Escape::form);
  body_.push_back('=');
  PercentEncoding::append(body_, value, Escape::form);
  return *this;
}

}  // namespace libchiaking::net::http
