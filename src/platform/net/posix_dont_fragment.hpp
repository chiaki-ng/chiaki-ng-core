/// @file
/// @brief Declares how the POSIX network stack sets the don't-fragment bit on
/// UDP sockets, which every system spells differently.
///
/// Senkusha, the network calibration before a stream, measures the path MTU
/// with datagrams that must be dropped rather than fragmented. Linux uses
/// path-MTU discovery modes, BSDs and macOS `IP_DONTFRAG`, and some systems
/// `IP_DONTFRAGMENT`; IPv6 uses `IPV6_DONTFRAG` everywhere it exists.
#pragma once

#include <netinet/in.h>
#include <sys/socket.h>

#include <libchiaking/detail/expected.hpp>
#include <libchiaking/platform/net/error.hpp>

#include <cerrno>
#include <cstdint>

namespace libchiaking::platform::detail {

/// @brief Tells whether a `setsockopt` failure means the option does not
/// exist on this system or socket, rather than a real error.
///
/// @param error_number The `errno` after the failure.
/// @return True for `ENOPROTOOPT`, `EINVAL`, `EOPNOTSUPP` and `ENOTSUP`.
constexpr bool dont_fragment_option_missing(int error_number) noexcept {
  return error_number == ENOPROTOOPT || error_number == EINVAL ||
         error_number == EOPNOTSUPP
#if defined(ENOTSUP) && ENOTSUP != EOPNOTSUPP
         || error_number == ENOTSUP
#endif
      ;
}

/// @brief Answers a don't-fragment request the system cannot carry out.
///
/// Turning the bit off is always satisfied, since without the option the
/// system does not set it.
///
/// @param enabled Whether the bit was to be set.
/// @return Nothing when `enabled` is false.
/// @retval NetError::not_supported The bit was to be set.
[[nodiscard]] inline auto dont_fragment_unavailable(bool enabled)
    -> Expected<void, NetError> {
  if (enabled) return unexpected(NetError::not_supported);
  return Expected<void, NetError>{};
}

/// @brief Sets or clears the don't-fragment bit with whichever option the
/// system has.
///
/// @tparam SetOption A callable with `setsockopt`'s last four arguments,
///     returning 0 on success and setting `errno` on failure.
/// @tparam LastError A callable returning the NetError for the current
///     `errno`.
/// @param level `IPPROTO_IP` or `IPPROTO_IPV6`, matching the socket's
///     family.
/// @param enabled Whether to set the bit.
/// @param set_option Sets the option on the socket.
/// @param last_error Reads the failure.
/// @return Nothing on success, or when clearing a bit the system has no
///     option for.
/// @retval NetError::not_supported The bit was to be set and the system or
///     socket has no option for it.
/// @retval NetError Any other `setsockopt` failure, as `last_error` gives
///     it.
template <typename SetOption, typename LastError>
[[nodiscard]] auto apply_dont_fragment(int level, bool enabled,
                                       SetOption&& set_option,
                                       LastError&& last_error)
    -> Expected<void, NetError> {
  const int value = enabled ? 1 : 0;
  auto finish = [&](int result) -> Expected<void, NetError> {
    if (result != 0) {
      if (dont_fragment_option_missing(errno))
        return dont_fragment_unavailable(enabled);
      return unexpected(last_error());
    }
    return Expected<void, NetError>{};
  };
#ifdef IP_DONTFRAGMENT
  if (level == IPPROTO_IP) {
    const auto value32 = static_cast<std::uint32_t>(enabled ? 1u : 0u);
    return finish(set_option(level, IP_DONTFRAGMENT, &value32,
                             static_cast<socklen_t>(sizeof(value32))));
  }
#endif
#ifdef IP_DONTFRAG
  if (level == IPPROTO_IP)
    return finish(set_option(level, IP_DONTFRAG, &value,
                             static_cast<socklen_t>(sizeof(value))));
#endif
#ifdef IP_MTU_DISCOVER
  if (level == IPPROTO_IP) {
    const int mtu_mode = enabled ? IP_PMTUDISC_DO : IP_PMTUDISC_DONT;
    return finish(set_option(level, IP_MTU_DISCOVER, &mtu_mode,
                             static_cast<socklen_t>(sizeof(mtu_mode))));
  }
#endif
#ifdef IPV6_DONTFRAG
  if (level == IPPROTO_IPV6)
    return finish(set_option(level, IPV6_DONTFRAG, &value,
                             static_cast<socklen_t>(sizeof(value))));
#endif
  (void)value;
  (void)set_option;
  (void)finish;
  return dont_fragment_unavailable(enabled);
}

/// @brief Probes whether a socket can have the don't-fragment bit set, by
/// setting it.
///
/// Leaves the bit set on success; probe a throw-away socket.
///
/// @tparam SetOption As for apply_dont_fragment().
/// @param level `IPPROTO_IP` or `IPPROTO_IPV6`.
/// @param set_option Sets the option on the probe socket.
/// @return True when setting the bit succeeded.
template <typename SetOption>
bool dont_fragment_available(int level, SetOption&& set_option) {
  return apply_dont_fragment(level, true, set_option,
                             [] { return NetError::system_failure; })
      .has_value();
}

}  // namespace libchiaking::platform::detail
