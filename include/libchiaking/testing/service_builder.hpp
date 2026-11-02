/// @file
/// @brief Declares ServiceBuilder, which builds a service the way ChiakiNg
/// does so a test can wire it to fakes.
#pragma once

#include <memory>
#include <utility>

namespace libchiaking::testing {

/// @brief Builds users::SignedInUsers, consoles::KnownConsoles,
/// catalog::Catalog, trophies::Trophies, play::Play and kakusan::Kakusan
/// from providers and stores a test chooses.
///
/// A frontend gets every service from ChiakiNg, whose constructors are the
/// only other way in. A test that needs one service on its own, wired to
/// fakes, builds it here instead.
/// @ingroup api_testing
class ServiceBuilder {
 public:
  /// @brief Builds a service in place.
  /// @tparam Service The service to build.
  /// @tparam Arguments The service's wiring arguments.
  /// @param arguments Forwarded to the service's wiring constructor.
  /// @return The service.
  template <class Service, class... Arguments>
  static auto build(Arguments&&... arguments) -> Service {
    return Service(std::forward<Arguments>(arguments)...);
  }

  /// @brief Builds a service on the heap, owned by one pointer.
  /// @tparam Service The service to build.
  /// @tparam Arguments The service's wiring arguments.
  /// @param arguments Forwarded to the service's wiring constructor.
  /// @return The service.
  template <class Service, class... Arguments>
  static auto build_unique(Arguments&&... arguments)
      -> std::unique_ptr<Service> {
    return std::unique_ptr<Service>(
        new Service(std::forward<Arguments>(arguments)...));
  }

  /// @brief Builds a service on the heap, owned by shared pointers.
  /// @tparam Service The service to build.
  /// @tparam Arguments The service's wiring arguments.
  /// @param arguments Forwarded to the service's wiring constructor.
  /// @return The service.
  template <class Service, class... Arguments>
  static auto build_shared(Arguments&&... arguments)
      -> std::shared_ptr<Service> {
    return std::shared_ptr<Service>(
        new Service(std::forward<Arguments>(arguments)...));
  }

  /// @brief Opens a library handle the way ChiakiNg::open does, with play's
  /// starts using collaborators the test chooses.
  /// @tparam Library ChiakiNg.
  /// @tparam Arguments The platform, the stores and the start collaborators.
  /// @param arguments Forwarded to the library's opener.
  /// @return The handle, or the missing provider.
  template <class Library, class... Arguments>
  static auto open_starting_with(Arguments&&... arguments) {
    return Library::open_starting_with(std::forward<Arguments>(arguments)...);
  }

  /// @brief Builds a Kakusan context the way ChiakiNg::kakusan does.
  /// @tparam Context kakusan::Kakusan.
  /// @tparam Arguments The platform, the device store and optionally the
  ///     STUN servers.
  /// @param arguments Forwarded to the context's factory.
  /// @return The context, or why Kakusan is unavailable.
  template <class Context, class... Arguments>
  static auto create(Arguments&&... arguments) {
    return Context::create(std::forward<Arguments>(arguments)...);
  }

  /// @brief Wraps an assembled Kakusan environment in a context.
  /// @tparam Context kakusan::Kakusan.
  /// @tparam Surroundings The environment's type.
  /// @param surroundings The environment to share.
  /// @return The context.
  template <class Context, class Surroundings>
  static auto on(Surroundings&& surroundings) -> Context {
    return Context::on(std::forward<Surroundings>(surroundings));
  }
};

}  // namespace libchiaking::testing
