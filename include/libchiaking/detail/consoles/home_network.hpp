/// @file
/// @brief Declares HomeNetwork, the choice of network provider one call
/// reaches a console on the home network through.
#pragma once

#include <memory>
#include <utility>

namespace libchiaking::platform {
class NetworkProvider;
}

namespace libchiaking::consoles {

/// @brief Names the network provider one call reaches a console through, in
/// place of the platform's own.
///
/// It lets a frontend reach a console over a network of its own making, such
/// as a tunnel or a VPN it implements as a platform::NetworkProvider, without
/// changing anything the library keeps about the console. Console::wake,
/// Console::wake_and_wait and Console::check take one for the call, a
/// SearchRanges subnet takes one for the search, and play::HomeOptions is
/// this type for a home stream, whose wakes and presence check use it too.
/// Nothing is saved with it: the console, its saved address and the
/// ConsoleStore are untouched, and every other call keeps the platform's
/// provider.
///
/// A provider whose platform::NetworkCapabilities say it cannot broadcast is
/// sent only unicast: to the console's own address, or to each address of the
/// subnet that names it.
class HomeNetwork {
 public:
  /// @brief Reaches the console over the platform's own network provider.
  HomeNetwork() noexcept = default;
  /// @brief Reaches the console over the given network provider.
  ///
  /// @param network Provider every socket, poller, name lookup and interface
  ///     listing of the call goes through instead of
  ///     platform::PlatformFactory::network, or null for the platform's own.
  ///     The call shares ownership and keeps it until the call, or the search
  ///     or session it started, has ended, so it may be used from the
  ///     library's threads for all that time.
  explicit HomeNetwork(
      std::shared_ptr<platform::NetworkProvider> network) noexcept
      : network_(std::move(network)) {}

  /// @brief Returns the provider the call runs over.
  ///
  /// @return The provider given, or null for the platform's own.
  auto network() const noexcept
      -> const std::shared_ptr<platform::NetworkProvider>& {
    return network_;
  }
  /// @brief Returns the provider a call runs over, given the platform's own.
  ///
  /// @param own The platform's provider.
  /// @return The provider given at construction, or @p own when none was.
  auto provider_or(platform::NetworkProvider& own) const noexcept
      -> platform::NetworkProvider& {
    return network_ ? *network_ : own;
  }

  /// @brief Compares the providers by identity.
  friend auto operator==(const HomeNetwork&, const HomeNetwork&) noexcept
      -> bool = default;

 private:
  std::shared_ptr<platform::NetworkProvider> network_;
};

}  // namespace libchiaking::consoles
