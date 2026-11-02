/// @file
/// @brief Declares ApiName, the human-readable name the diagnostics log and
/// API captures give each HTTP request, such as "PSN List consoles".
#pragma once

#include "libchiaking/platform/http/provider.hpp"

#include <span>
#include <string_view>

namespace libchiaking::diagnostics {

/// @brief The display name of the PS or third-party API an HTTP request
/// calls, worked out from the request's label and URL.
///
/// A request whose label is in known() gets that entry's name; other PSN
/// GraphQL labels get a generic mobile or web query name; the STUN server
/// list downloads are recognised by URL; anything else is "Unclassified
/// HTTP API". The name always refers to static storage, so the object may
/// outlive the request. A plain value; copy freely.
class ApiName {
 public:
  /// @brief One known request label and its display name.
  struct Entry {
    /// @brief The label the client sets on the request, such as
    /// `psn.gamelist.played_titles`.
    std::string_view label;
    /// @brief The name shown for it, such as "PSN List played titles".
    std::string_view name;
  };

  /// @brief Names a request.
  ///
  /// @param request The request; read only during construction.
  explicit ApiName(const platform::HttpRequest& request) noexcept;

  /// @brief Returns the name.
  ///
  /// @return The display name, in static storage.
  auto view() const noexcept -> std::string_view { return name_; }
  /// @brief Returns the name as a C string, for C callbacks.
  ///
  /// @return The display name, NUL-terminated, in static storage.
  auto c_str() const noexcept -> const char* { return name_.data(); }

  /// @brief Lists every label with a name of its own.
  ///
  /// @return The table, in static storage.
  static auto known() noexcept -> std::span<const Entry>;

 private:
  /// @brief Works out the name of a request.
  ///
  /// @param request The request.
  /// @return The display name, in static storage.
  static auto resolve(const platform::HttpRequest& request) noexcept
      -> std::string_view;

  /// @brief The display name.
  std::string_view name_;
};

}  // namespace libchiaking::diagnostics
