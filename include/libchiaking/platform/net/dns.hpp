/// @file
/// @brief Declares DnsProvider, the port that resolves host names to addresses.
#pragma once

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>
#include <libchiaking/platform/net/types.hpp>
#include <libchiaking/platform/runtime/threading.hpp>

#include <string_view>
#include <vector>

namespace libchiaking::platform {

/// @brief Port that resolves host names to IP addresses.
///
/// Implemented by the host frontend, or by DefaultDnsProvider in this tree. The
/// library reaches it through NetworkProvider::dns to find consoles given by
/// name, STUN servers and cloud streaming servers.
///
/// Both overloads are synchronous: the calling library thread waits for the
/// answer.
class DnsProvider {
 public:
  /// @brief Allows destruction through the interface.
  virtual ~DnsProvider() = default;
  /// @brief Resolves a host name, blocking until the resolver answers.
  ///
  /// @param host Host name or numeric address. A numeric address must resolve
  ///     to itself.
  /// @param service Port number or service name passed to the resolver, or
  ///     empty for none.
  /// @return Every address found, IPv4 and IPv6, in resolver order. Never
  ///     empty on success.
  /// @retval NetError::host_unreachable The name does not exist or has no
  ///     usable address.
  /// @retval NetError::timed_out The resolver reported a temporary failure.
  /// @retval NetError::no_buffers The resolver ran out of memory.
  [[nodiscard]] virtual auto resolve(std::string_view host,
                                     std::string_view service)
      -> Expected<std::vector<IpAddress>, NetError> = 0;
  /// @brief Resolves a host name, giving up when the stop token is stopped.
  ///
  /// The default checks the token once and then calls the blocking overload,
  /// so it cannot abandon a lookup that is already running. A provider that
  /// can cancel should override it, as DefaultDnsProvider does.
  ///
  /// @param host Host name or numeric address.
  /// @param service Port number or service name, or empty for none.
  /// @param stop Cancels the lookup when stopped.
  /// @return Every address found.
  /// @retval NetError::timed_out The stop token's deadline expired.
  /// @retval NetError::interrupted The stop token was stopped for another
  ///     reason.
  [[nodiscard]] virtual auto resolve(std::string_view host,
                                     std::string_view service,
                                     const StopToken& stop)
      -> Expected<std::vector<IpAddress>, NetError> {
    if (stop.stop_requested())
      return unexpected(stop.deadline_expired() ? NetError::timed_out
                                                : NetError::interrupted);
    return resolve(host, service);
  }
};

}  // namespace libchiaking::platform
