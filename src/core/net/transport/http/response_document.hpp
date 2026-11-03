/// @file
/// @brief Defines Response::document(), which needs the JSON parser that the
/// rest of Response's users do not.
#pragma once

#include "src/core/net/transport/http/response.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"

namespace libchiaking::net::http {

template <class Document>
auto Response::document() const -> Expected<Document, json::ParseError> {
  return json::read<Document>(text::c_string(body()), maximum_json_bytes_);
}

}  // namespace libchiaking::net::http
