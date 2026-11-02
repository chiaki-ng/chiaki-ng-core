/// @file
/// @brief Declares StoreError, the failure of a LegacyCloudStream store
/// request: the Kamaji commerce store or the LegacyCloudStream game lists.
#pragma once

#include "src/core/net/transport/http/failure.hpp"

#include <cstdint>
#include <optional>

namespace libchiaking::psn::rest {

/// @brief Why a LegacyCloudStream store request failed, with the reply's status
/// and, for a failure to send, the transport failure. An immutable value.
class StoreError {
 public:
  /// @brief Why the request failed.
  enum class Reason : std::uint8_t {
    /// @brief The request did not complete, or could not be built.
    failed,
    /// @brief The store answered with an unexpected status, or a Kamaji
    /// `header.status_code` other than kamaji_status_ok.
    refused,
    /// @brief The reply could not be read or lacked what was asked for.
    malformed,
    /// @brief The Kamaji store refused the product with
    /// kamaji_status_ineligible: it is sold in another store region than the
    /// account's.
    ineligible,
  };

  /// @brief Wraps a failure to send.
  ///
  /// @param failure The client's failure.
  explicit StoreError(net::http::Failure failure) noexcept
      : reason_(Reason::failed), failure_(failure) {}
  /// @brief Wraps a refusal or an unreadable reply.
  ///
  /// @param reason Why.
  /// @param http_status The reply's status.
  StoreError(Reason reason, std::uint16_t http_status) noexcept
      : reason_(reason), http_status_(http_status) {}

  /// @brief Reads why the request failed.
  ///
  /// @return The reason.
  auto reason() const noexcept -> Reason { return reason_; }
  /// @brief Reads the reply's status.
  ///
  /// @return It, or 0 for a failure to send.
  auto http_status() const noexcept -> std::uint16_t { return http_status_; }
  /// @brief Reads the failure to send.
  ///
  /// @return It, or nothing when the store answered.
  auto failure() const noexcept -> const std::optional<net::http::Failure>& {
    return failure_;
  }

 private:
  /// @brief Why.
  Reason reason_;
  /// @brief The reply's status; 0 for a failure to send.
  std::uint16_t http_status_{};
  /// @brief The failure to send.
  std::optional<net::http::Failure> failure_;
};

}  // namespace libchiaking::psn::rest
