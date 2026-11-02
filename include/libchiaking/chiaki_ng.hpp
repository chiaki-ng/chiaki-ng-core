/// @file
/// @brief Declares ChiakiNg, the entry point a frontend constructs to use the
/// library.
#pragma once

#include "libchiaking/catalog.hpp"
#include "libchiaking/consoles.hpp"
#include "libchiaking/detail/diagnostics/http_call_log.hpp"
#include "libchiaking/kakusan.hpp"
#include "libchiaking/messages.hpp"
#include "libchiaking/play.hpp"
#include "libchiaking/trophies.hpp"
#include "libchiaking/users.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::platform {
class RecordStore;
}

namespace libchiaking::platform {
class PlatformFactory;
}

namespace libchiaking::play {
struct StartCollaborators;
}

namespace libchiaking::testing {
class ServiceBuilder;
}

namespace libchiaking {

/// @brief Wires the library's bounded contexts together and hands each one to
/// the frontend.
///
/// A frontend opens exactly one ChiakiNg with ChiakiNg::open, giving it the
/// platform services and the persistent stores, and then reaches every feature
/// through the accessors: users() for PlayStation Network accounts, consoles()
/// for paired and discovered consoles, catalog() for games and cloud services,
/// play() for sessions, and kakusan() for sharing a session with remote
/// guests. A build without Kakusan still offers ChiakiNg; kakusan() then
/// reports kakusan::Kakusan::Unavailable::not_built.
///
/// ChiakiNg is a cheap handle. Copies share the same wiring, and the references
/// returned by the accessors stay valid for as long as any copy is alive.
///
/// @warning The platform factory and every store passed to ChiakiNg::open are
///     held by reference. They must outlive every copy of the ChiakiNg and
///     everything obtained from it. The stock platform that
///     ChiakiNg::open(Stores, Settings) opens with is owned by the library.
/// @ingroup api_start
class ChiakiNg {
 public:
  /// @brief Names the platform service whose absence stopped ChiakiNg::open.
  enum class Missing : std::uint8_t {
    /// @brief The platform factory has no HTTP provider.
    http,
    /// @brief The platform factory has no crypto provider.
    crypto,
  };

  /// @brief The persistent stores the frontend supplies; each is implemented
  /// by the frontend and owned by it.
  ///
  /// The library keeps the references and the pointer for its whole lifetime
  /// and never takes ownership.
  struct Stores {
    /// @brief Where signed-in accounts and their credentials are kept
    /// between runs.
    users::CredentialStore& credentials;
    /// @brief Where the consoles this device knows are kept between runs.
    consoles::ConsoleStore& consoles;
    /// @brief Where remembered cloud datacenter choices are kept.
    play::DatacenterStore& datacenters;
    /// @brief Where Kakusan keeps this device's identity and remembered
    /// devices, or null when the frontend does not offer Kakusan.
    ///
    /// With a null pointer ChiakiNg::kakusan and
    /// ChiakiNg::use_stun_servers answer
    /// kakusan::Kakusan::Unavailable::device_store_unavailable;
    /// everything else works. Ignored by a build without Kakusan.
    kakusan::DeviceStore* devices;
    /// @brief The record store that backs the catalog's caches and the
    /// trophies kept between runs.
    platform::RecordStore& records;
  };

  /// @brief Optional settings; a value-initialised Settings is valid.
  struct Settings {
    /// @brief STUN servers Kakusan uses to find a public address, as
    /// host names or addresses.
    ///
    /// The list is checked only when Kakusan is first built, so an
    /// unusable list surfaces from ChiakiNg::kakusan, not from
    /// ChiakiNg::open. At most kakusan::Kakusan::most_stun_servers
    /// entries, each non-empty and no longer than
    /// kakusan::Kakusan::longest_stun_server.
    std::vector<std::string> stun_servers;
    /// @brief Journal that play sessions are recorded in, or null to keep
    /// no journal.
    std::shared_ptr<diagnostics::SessionJournal> journal;
  };

  /// @brief Builds every bounded context on top of the given platform and
  /// stores.
  ///
  /// @param platform Source of the platform providers (network, HTTP, crypto,
  ///     clock, randomness, diagnostics). Held by reference.
  /// @param stores The frontend's persistent stores. Held by reference.
  /// @param settings Optional settings; see Settings.
  /// @return The ready library handle.
  /// @retval Missing::http The platform does not supply
  ///     PlatformCapability::http.
  /// @retval Missing::crypto The platform does not supply
  ///     PlatformCapability::crypto.
  /// @note Kakusan is not built here. It is built on the first call to
  ///     kakusan() or use_stun_servers().
  [[nodiscard]] static auto open(platform::PlatformFactory& platform,
                                 Stores stores, Settings settings = {})
      -> Expected<ChiakiNg, Missing>;
  /// @brief Builds every bounded context on the host's stock platform and
  /// the given stores.
  ///
  /// The stock platform is the one platform::DefaultPlatform assembles for
  /// this host: its threading, clocks, randomness, network, crypto, curl HTTP
  /// and WebSocket providers. The library owns it and keeps it alive for as
  /// long as any copy of the returned ChiakiNg, or anything obtained from it,
  /// is.
  ///
  /// @param stores The frontend's persistent stores. Held by reference.
  /// @param settings Optional settings; see Settings.
  /// @return The ready library handle.
  /// @retval Missing::http The library was built without a stock platform
  ///     for this host, so there is no HTTP provider to open with.
  /// @note Kakusan is not built here. It is built on the first call to
  ///     kakusan() or use_stun_servers().
  [[nodiscard]] static auto open(Stores stores, Settings settings = {})
      -> Expected<ChiakiNg, Missing>;

  /// @brief The signed-in PlayStation Network accounts.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto users() noexcept -> users::SignedInUsers&;
  /// @brief The consoles this device knows: paired ones and ones found on the
  /// network.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto consoles() noexcept -> consoles::KnownConsoles&;
  /// @brief The game catalogue and cloud service answers, with their caches.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto catalog() noexcept -> catalog::Catalog&;
  /// @brief The signed-in players' trophy levels, trophy lists and each
  /// title's trophies, kept in Stores::records.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto trophies() noexcept -> trophies::Trophies&;
  /// @brief The signed-in players' message conversations, read and written
  /// with the app token; nothing is kept between calls.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto messages() noexcept -> messages::Messages&;
  /// @brief The context that starts and owns play sessions, at home, over the
  /// internet and in the cloud.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives.
  auto play() noexcept -> play::Play&;
  /// @brief The Kakusan context, built on first use.
  ///
  /// The first successful call loads this device's Kakusan identity from
  /// Stores::devices, creating and saving one if none exists, and loads the
  /// remembered devices. Later calls return a handle to the same context. A
  /// failed call builds nothing, so the next call tries again. Calls are
  /// serialised and may come from any thread.
  ///
  /// @return A handle to the one Kakusan context.
  /// @retval kakusan::Kakusan::Unavailable::not_built The library was built
  ///     without Kakusan (`LIBCHIAKING_KAKUSAN=OFF`); every call answers
  ///     this, before any other check.
  /// @retval kakusan::Kakusan::Unavailable::device_store_unavailable
  ///     Stores::devices is null, or the store failed to load or save.
  /// @retval kakusan::Kakusan::Unavailable::no_secure_transport The
  ///     platform has no secure datagram provider.
  /// @retval kakusan::Kakusan::Unavailable::no_identity_crypto The
  ///     crypto provider lacks a primitive Kakusan needs, or the identity
  ///     could not be restored or created.
  /// @retval kakusan::Kakusan::Unavailable::invalid_stun_servers The
  ///     configured STUN server list is not usable.
  /// @see kakusan::Kakusan::create
  [[nodiscard]] auto kakusan()
      -> Expected<kakusan::Kakusan, kakusan::Kakusan::Unavailable>;
  /// @brief Replaces the STUN servers Kakusan uses.
  ///
  /// Takes effect for shared sessions and visits started afterwards; ones
  /// already running keep the servers they started with. If Kakusan has
  /// not been built yet this builds it with the given list, so every failure
  /// of kakusan() can come back from here too. On failure the previous
  /// list is kept.
  ///
  /// @param servers The new list, replacing the old one. May be empty.
  /// @return Nothing on success.
  /// @retval kakusan::Kakusan::Unavailable::not_built The library was built
  ///     without Kakusan (`LIBCHIAKING_KAKUSAN=OFF`); the list is not kept.
  /// @retval kakusan::Kakusan::Unavailable::invalid_stun_servers More
  ///     than kakusan::Kakusan::most_stun_servers entries, or an entry
  ///     that is empty or longer than
  ///     kakusan::Kakusan::longest_stun_server.
  /// @retval kakusan::Kakusan::Unavailable::device_store_unavailable
  ///     Stores::devices is null, or the store failed while Kakusan was
  ///     being built.
  [[nodiscard]] auto use_stun_servers(std::span<const std::string> servers)
      -> Expected<void, kakusan::Kakusan::Unavailable>;

  /// @brief Lists the catalog's caches for a storage settings screen.
  /// @return One summary per cache, or the reason the catalog could not
  ///     answer.
  /// @see catalog::Catalog::caches
  [[nodiscard]] auto caches()
      -> Expected<std::vector<catalog::CacheSummary>, catalog::CachesFailure>;
  /// @brief Lists the entries held in one catalog cache.
  /// @param cache The cache to look into, as named by a
  ///     catalog::CacheSummary.
  /// @return The entries, or the reason the catalog could not answer.
  /// @see catalog::Catalog::cache_entries
  [[nodiscard]] auto cache_entries(catalog::CacheId cache)
      -> Expected<std::vector<catalog::CacheEntry>,
                  catalog::CacheEntriesFailure>;
  /// @brief Empties one catalog cache.
  /// @param cache The cache to empty.
  /// @return Nothing on success, or the reason the catalog could not do it.
  /// @see catalog::Catalog::clear_cache
  [[nodiscard]] auto clear_cache(catalog::CacheId cache)
      -> Expected<void, catalog::ClearCacheFailure>;
  /// @brief Empties every catalog cache.
  /// @return Nothing on success, or the reason the catalog could not do it.
  /// @see catalog::Catalog::clear_caches
  [[nodiscard]] auto clear_caches()
      -> Expected<void, catalog::ClearCachesFailure>;

  /// @brief The record of the HTTP exchanges the library has made.
  ///
  /// Every call made through users(), consoles(), catalog(), trophies(),
  /// messages(), play() and kakusan() is recorded, tagged with the context that
  /// made it and the credentials it carried. The log is always on, kept in
  /// memory only, and bounded; see diagnostics::HttpCallLog.
  /// @return A reference that stays valid while any copy of this ChiakiNg
  ///     lives. Safe to use from any thread.
  auto http_calls() noexcept -> diagnostics::HttpCallLog&;

  /// @brief Signs an account out of the whole library.
  ///
  /// Leaves the live play session if it is being played as this account,
  /// drops what the catalog and trophies hold for the account, and then
  /// removes the account and its credentials. The first steps happen even
  /// when the last one fails.
  ///
  /// @param account The account to sign out.
  /// @return Nothing on success.
  /// @retval users::CredentialStore::Error The credential store could not
  ///     remove the account; the value is the store's own error.
  [[nodiscard]] auto sign_out(AccountId account)
      -> Expected<void, users::CredentialStore::Error>;

 private:
  friend class testing::ServiceBuilder;

  class Wiring;

  explicit ChiakiNg(std::shared_ptr<Wiring> wiring) noexcept
      : wiring_(std::move(wiring)) {}

  [[nodiscard]] static auto open_owning(std::shared_ptr<void> owner,
                                        platform::PlatformFactory& platform,
                                        Stores stores, Settings settings)
      -> Expected<ChiakiNg, Missing>;
  /// @brief Builds every bounded context as open does, with play's starts
  /// using the given collaborators; reached through testing::ServiceBuilder.
  ///
  /// @param platform Source of the platform providers. Held by reference.
  /// @param stores The persistent stores. Held by reference.
  /// @param start What play's starts clear titles, waits and keep time with.
  /// @return The ready library handle, or the missing provider as open.
  [[nodiscard]] static auto open_starting_with(
      platform::PlatformFactory& platform, Stores stores,
      const play::StartCollaborators& start) -> Expected<ChiakiNg, Missing>;

  std::shared_ptr<Wiring> wiring_;
};

}  // namespace libchiaking
