/// @file
/// @brief Declares Response, an HTTP response received through Client.
#pragma once

#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/http/provider.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::net::http {

/// @brief An HTTP response the client received, with any status, and how many
/// attempts it took.
///
/// Owns the platform response, whose body is held as secret bytes. The JSON
/// reader refuses bodies larger than the request's size limit. Not shared
/// between threads while being read and released.
class Response {
 public:
  /// @brief Holds a response.
  ///
  /// @param response The platform response.
  /// @param attempts Attempts the request took, from 1.
  /// @param maximum_json_bytes Largest body document() parses, in bytes.
  Response(platform::HttpResponse response, int attempts,
           std::size_t maximum_json_bytes) noexcept;

  /// @brief The HTTP status.
  ///
  /// @return The status code.
  auto status() const noexcept -> std::uint16_t {
    return response_.status_code;
  }
  /// @brief How many attempts the request took.
  ///
  /// @return From 1 to max_retries plus one.
  auto attempts() const noexcept -> int { return attempts_; }
  /// @brief Finds a response header, ignoring the case of its name.
  ///
  /// @param name The header name.
  /// @return The first such header's value; nothing when absent.
  [[nodiscard]] auto header(std::string_view name) const noexcept
      -> std::optional<std::string_view>;
  /// @brief Every response header, in the order received.
  ///
  /// @return A view valid while the response lives.
  auto headers() const noexcept -> std::span<const platform::HttpHeader> {
    return response_.headers;
  }
  /// @brief The body as text.
  ///
  /// @return A view valid while the response lives.
  auto body() const noexcept -> std::string_view {
    return response_.body.view();
  }
  /// @brief The body as bytes.
  ///
  /// @return A view valid while the response lives.
  auto bytes() const noexcept -> std::span<const std::byte> {
    return response_.body.bytes();
  }
  /// @brief The URL the response came from; the request URL, since redirects
  /// are never followed.
  ///
  /// @return A view valid while the response lives.
  auto final_url() const noexcept -> std::string_view {
    return response_.final_url.view();
  }
  /// @brief Whether the body is larger than document() will parse.
  ///
  /// @return True when the body exceeds the size limit.
  auto oversized() const noexcept -> bool {
    return response_.body.size() > maximum_json_bytes_;
  }
  /// @brief Parses the body, up to its first NUL, as a JSON document.
  ///
  /// @tparam Document A type json::read can fill.
  /// @return The document.
  /// @retval json::ParseError The body is too large or not a valid document.
  template <class Document>
  [[nodiscard]] auto document() const -> Expected<Document, json::ParseError> {
    return json::read<Document>(text::c_string(body()), maximum_json_bytes_);
  }
  /// @brief Hands over the platform response.
  ///
  /// @return The response.
  auto release() && noexcept -> platform::HttpResponse {
    return std::move(response_);
  }

 private:
  /// @brief The platform response.
  platform::HttpResponse response_;
  /// @brief Attempts the request took.
  int attempts_;
  /// @brief Largest body document() parses, in bytes.
  std::size_t maximum_json_bytes_;
};

}  // namespace libchiaking::net::http
