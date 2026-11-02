/// @file
/// @brief Declares Query and Form, the builders of request URLs and form
/// bodies.
#pragma once

#include "src/core/net/transport/http/percent_encoding.hpp"

#include <libchiaking/detail/secret_string.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::net::http {

/// @brief A URL being built: a base URL with query parameters appended one by
/// one.
///
/// Held in a SecretString because query values are often credentials. Names
/// are appended as given; values are percent-encoded unless added verbatim.
/// Not shared between threads.
class Query {
 public:
  /// @brief Starts from a base URL.
  ///
  /// @param base The URL; when it already holds a `?`, parameters continue
  ///     with
  ///     `&`.
  explicit Query(std::string_view base);

  /// @brief Appends `name=value`, percent-encoding the value.
  ///
  /// @param name The parameter name, appended as is.
  /// @param value The value.
  /// @param escape The rule set for the value.
  /// @return This query.
  auto add(std::string_view name, std::string_view value,
           Escape escape = Escape::component) -> Query&;
  /// @brief Appends `name=value` without encoding the value, for values that
  /// are already encoded.
  ///
  /// @param name The parameter name.
  /// @param value The value, appended as is.
  /// @return This query.
  auto add_verbatim(std::string_view name, std::string_view value) -> Query&;

  /// @brief The URL so far.
  ///
  /// @return A view valid until the query changes.
  auto view() const noexcept -> std::string_view { return url_.view(); }
  /// @brief Hands over the finished URL.
  ///
  /// @return The URL.
  auto take() && noexcept -> SecretString { return std::move(url_); }

 private:
  /// @brief Appends `?` or `&`, the name and `=`.
  ///
  /// @param name The parameter name.
  void separate(std::string_view name);

  /// @brief The URL so far.
  SecretString url_;
  /// @brief Whether the URL already holds a `?`.
  bool has_query_{};
};

/// @brief An `application/x-www-form-urlencoded` body being built.
///
/// Held in a SecretString because form fields often carry credentials. Not
/// shared between threads.
class Form {
 public:
  /// @brief Appends `name=value`, both form-encoded, separated from earlier
  /// fields by `&`.
  ///
  /// @param name The field name.
  /// @param value The field value.
  /// @return This form.
  auto add(std::string_view name, std::string_view value) -> Form&;

  /// @brief The body so far.
  ///
  /// @return A view valid until the form changes.
  auto view() const noexcept -> std::string_view { return body_.view(); }
  /// @brief Hands over the finished body.
  ///
  /// @return The body.
  auto take() && noexcept -> SecretString { return std::move(body_); }

 private:
  /// @brief The body so far.
  SecretString body_;
};

}  // namespace libchiaking::net::http
