/// @file
/// @brief Declares HttpStatus, an HTTP status code and the classes of status
/// the library acts on.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace libchiaking::net {

/// @brief An HTTP status code, with the classes of status the library acts
/// on.
///
/// A plain value; copy freely.
class HttpStatus {
 public:
  /// @brief The largest code a 16-bit status field holds.
  static constexpr std::uint16_t largest_code = 0xFFFF;

  /// @brief HTTP 200 OK.
  static const HttpStatus ok;
  /// @brief HTTP 201 Created.
  static const HttpStatus created;
  /// @brief HTTP 204 No Content.
  static const HttpStatus no_content;
  /// @brief HTTP 302 Found, the redirect PS's sign-in answers with.
  static const HttpStatus found;
  /// @brief HTTP 400 Bad Request.
  static const HttpStatus bad_request;
  /// @brief HTTP 401 Unauthorized.
  static const HttpStatus unauthorized;
  /// @brief HTTP 403 Forbidden.
  static const HttpStatus forbidden;
  /// @brief HTTP 404 Not Found.
  static const HttpStatus not_found;
  /// @brief HTTP 429 Too Many Requests, which the client retries by default.
  static const HttpStatus too_many_requests;

  /// @brief Holds a status code.
  ///
  /// @param code The code, as received.
  constexpr explicit HttpStatus(std::uint16_t code) noexcept : code_(code) {}

  /// @brief Returns the code.
  ///
  /// @return The status code.
  constexpr auto code() const noexcept -> std::uint16_t { return code_; }

  /// @brief Whether the status means success.
  ///
  /// @return True for 200 to 299.
  constexpr auto is_success() const noexcept -> bool {
    return code_ >= 200 && code_ < 300;
  }

  /// @brief Whether the status means the credential was refused.
  ///
  /// @return True for 401 and 403.
  constexpr auto is_auth_refused() const noexcept -> bool {
    return code_ == unauthorized_code || code_ == forbidden_code;
  }

  /// @brief Whether the status is a client error.
  ///
  /// @return True for 400 to 499.
  constexpr auto is_client_error() const noexcept -> bool {
    return code_ >= 400 && code_ < 500;
  }

  /// @brief Whether the status is a server error.
  ///
  /// @return True for 500 and above.
  constexpr auto is_server_error() const noexcept -> bool {
    return code_ >= 500;
  }

  /// @brief Compares two statuses by code.
  ///
  /// @return True when the codes are equal.
  friend constexpr auto operator==(HttpStatus, HttpStatus) noexcept
      -> bool = default;

 private:
  /// @brief The code of HTTP 401.
  static constexpr std::uint16_t unauthorized_code = 401;
  /// @brief The code of HTTP 403.
  static constexpr std::uint16_t forbidden_code = 403;

  /// @brief The status code.
  std::uint16_t code_;
};

inline constexpr HttpStatus HttpStatus::ok{200};
inline constexpr HttpStatus HttpStatus::created{201};
inline constexpr HttpStatus HttpStatus::no_content{204};
inline constexpr HttpStatus HttpStatus::found{302};
inline constexpr HttpStatus HttpStatus::bad_request{400};
inline constexpr HttpStatus HttpStatus::unauthorized{unauthorized_code};
inline constexpr HttpStatus HttpStatus::forbidden{forbidden_code};
inline constexpr HttpStatus HttpStatus::not_found{404};
inline constexpr HttpStatus HttpStatus::too_many_requests{429};

/// @brief The start of an HTTP/1.1 status line, as the console's ctrl and
/// session replies begin.
inline constexpr std::string_view http_version_prefix = "HTTP/1.1 ";
/// @brief Bytes of the CRLF CRLF that ends HTTP headers.
inline constexpr std::size_t http_header_terminator_size = 4;

}  // namespace libchiaking::net
