#include "libchiaking/chiaki_ng.hpp"

#include "src/core/catalog/ntp_clock.hpp"
#include "src/core/play/play_core.hpp"
#include "src/core/utils/diagnostics/recording_platform.hpp"

#include <algorithm>
#include <optional>

namespace libchiaking {

class ChiakiNg::Wiring {
  std::shared_ptr<void> platform_owner_;

 public:
  diagnostics::HttpCallLog http_calls;

 private:
  catalog::NtpClock trusted_time_;
  diagnostics::RecordingPlatform users_platform_;
  diagnostics::RecordingPlatform consoles_platform_;
  diagnostics::RecordingPlatform catalog_platform_;
  diagnostics::RecordingPlatform trophies_platform_;
  diagnostics::RecordingPlatform messages_platform_;
  diagnostics::RecordingPlatform play_platform_;
  users::RequestBudget psn_budget_;

 public:
  Wiring(std::shared_ptr<void> platform_owner,
         platform::PlatformFactory& platform, platform::CryptoProvider& crypto,
         const Stores& stores, Settings settings,
         std::optional<play::StartCollaborators> start)
      : platform_owner_(std::move(platform_owner)),
        http_calls(platform.threading()),
        trusted_time_(platform.network(), platform.threading(),
                      platform.clock(), platform.random()),
        users_platform_(platform, http_calls, diagnostics::HttpArea::users),
        consoles_platform_(platform, http_calls,
                           diagnostics::HttpArea::consoles),
        catalog_platform_(platform, http_calls, diagnostics::HttpArea::catalog),
        trophies_platform_(platform, http_calls,
                           diagnostics::HttpArea::trophies),
        messages_platform_(platform, http_calls,
                           diagnostics::HttpArea::messages),
        play_platform_(platform, http_calls, diagnostics::HttpArea::play),
        psn_budget_(platform.threading(), platform.clock()),
        users(users_platform_.recorded_http(), stores.credentials,
              platform.threading(), platform.clock(), platform.wall_clock(),
              platform.random(), sink_of(platform)),
        pairer(consoles_platform_, sink_of(platform)),
        consoles(platform.network(), consoles_platform_.recorded_http(), crypto,
                 platform.random(), stores.consoles, platform.threading(),
                 platform.clock(), &pairer, sink_of(platform)),
        catalog(catalog_platform_.recorded_http(), stores.records,
                platform.threading(), platform.clock(), platform.wall_clock(),
                trusted_time_, psn_budget_, sink_of(platform)),
        trophies(trophies_platform_.recorded_http(), stores.records,
                 platform.threading(), platform.clock(), platform.wall_clock(),
                 psn_budget_, sink_of(platform)),
        messages(messages_platform_.recorded_http(), platform.clock(),
                 platform.wall_clock(), sink_of(platform)),
        play(start ? std::make_unique<play::Play::Core>(
                         play_platform_, consoles, catalog, stores.datacenters,
                         pairer, sink_of(platform), std::move(*start))
                   : std::make_unique<play::Play::Core>(
                         play_platform_, consoles, catalog, stores.datacenters,
                         pairer, sink_of(platform))),
        platform_(play_platform_),
        devices_(stores.devices),
        mutex_(platform.threading()),
        stun_servers_(std::move(settings.stun_servers)) {
    consoles.start_through(&play.starting());
    consoles.watch_through(
        platform.supplies(platform::PlatformCapability::websocket)
            ? &platform.websocket()
            : nullptr);
    if (settings.journal) play.journal_sessions_in(std::move(settings.journal));
  }

  [[nodiscard]] static auto opened(
      std::shared_ptr<void> owner, platform::PlatformFactory& platform,
      const Stores& stores, Settings settings,
      std::optional<play::StartCollaborators> start)
      -> Expected<std::shared_ptr<Wiring>, Missing> {
    if (!platform.supplies(platform::PlatformCapability::http))
      return unexpected(Missing::http);
    if (!platform.supplies(platform::PlatformCapability::crypto))
      return unexpected(Missing::crypto);
    return std::make_shared<Wiring>(std::move(owner), platform,
                                    platform.crypto(), stores,
                                    std::move(settings), std::move(start));
  }

  Wiring(const Wiring&) = delete;
  auto operator=(const Wiring&) -> Wiring& = delete;

  ~Wiring() {
    consoles.watch_through(nullptr);
    consoles.start_through(nullptr);
  }

  [[nodiscard]] auto kakusan()
      -> Expected<kakusan::Kakusan, kakusan::Kakusan::Unavailable> {
    const platform::Lock lock{mutex_};
    if (!kakusan_) {
      auto created = share_through(platform_, devices_, stun_servers_);
      if (!created) return unexpected(created.error());
      kakusan_.emplace(std::move(created.value()));
    }
    return *kakusan_;
  }

  [[nodiscard]] auto use_stun_servers(std::span<const std::string> servers)
      -> Expected<void, kakusan::Kakusan::Unavailable> {
    const platform::Lock lock{mutex_};
    if (kakusan_) {
      if (auto changed = restun(*kakusan_, servers); !changed)
        return unexpected(changed.error());
      stun_servers_.assign(servers.begin(), servers.end());
      return {};
    }
    auto created = share_through(platform_, devices_, servers);
    if (!created) return unexpected(created.error());
    stun_servers_.assign(servers.begin(), servers.end());
    kakusan_.emplace(std::move(created.value()));
    return {};
  }

  users::SignedInUsers users;
  play::InternetPairer pairer;
  consoles::KnownConsoles consoles;
  catalog::Catalog catalog;
  trophies::Trophies trophies;
  messages::Messages messages;
  play::Play play;

 private:
  static auto sink_of(platform::PlatformFactory& platform) noexcept
      -> platform::DiagnosticsProvider* {
    return platform.supplies(platform::PlatformCapability::diagnostics)
               ? &platform.diagnostics()
               : nullptr;
  }

  [[nodiscard]] static auto share_through(platform::PlatformFactory& platform,
                                          kakusan::DeviceStore* devices,
                                          std::span<const std::string> servers)
      -> Expected<kakusan::Kakusan, kakusan::Kakusan::Unavailable> {
#if LIBCHIAKING_HAS_KAKUSAN
    if (devices == nullptr)
      return unexpected(
          kakusan::Kakusan::Unavailable::device_store_unavailable);
    return kakusan::Kakusan::create(platform, *devices, servers);
#else
    static_cast<void>(platform);
    static_cast<void>(devices);
    static_cast<void>(servers);
    return unexpected(kakusan::Kakusan::Unavailable::not_built);
#endif
  }

  [[nodiscard]] static auto restun(kakusan::Kakusan& shared,
                                   std::span<const std::string> servers)
      -> Expected<void, kakusan::Kakusan::Unavailable> {
#if LIBCHIAKING_HAS_KAKUSAN
    return shared.use_stun_servers(servers);
#else
    static_cast<void>(shared);
    static_cast<void>(servers);
    return unexpected(kakusan::Kakusan::Unavailable::not_built);
#endif
  }

  platform::PlatformFactory& platform_;
  kakusan::DeviceStore* devices_;
  platform::Mutex mutex_;
  std::vector<std::string> stun_servers_;
  std::optional<kakusan::Kakusan> kakusan_;
};

auto ChiakiNg::open(platform::PlatformFactory& platform, Stores stores,
                    Settings settings) -> Expected<ChiakiNg, Missing> {
  return open_owning(nullptr, platform, stores, std::move(settings));
}

auto ChiakiNg::open_owning(std::shared_ptr<void> owner,
                           platform::PlatformFactory& platform, Stores stores,
                           Settings settings) -> Expected<ChiakiNg, Missing> {
  auto wiring = Wiring::opened(std::move(owner), platform, stores,
                               std::move(settings), std::nullopt);
  if (!wiring) return unexpected(wiring.error());
  return ChiakiNg{std::move(wiring.value())};
}

auto ChiakiNg::open_starting_with(platform::PlatformFactory& platform,
                                  Stores stores,
                                  const play::StartCollaborators& start)
    -> Expected<ChiakiNg, Missing> {
  auto wiring = Wiring::opened(nullptr, platform, stores, Settings{}, start);
  if (!wiring) return unexpected(wiring.error());
  return ChiakiNg{std::move(wiring.value())};
}

auto ChiakiNg::users() noexcept -> users::SignedInUsers& {
  return wiring_->users;
}

auto ChiakiNg::consoles() noexcept -> consoles::KnownConsoles& {
  return wiring_->consoles;
}

auto ChiakiNg::catalog() noexcept -> catalog::Catalog& {
  return wiring_->catalog;
}

auto ChiakiNg::trophies() noexcept -> trophies::Trophies& {
  return wiring_->trophies;
}

auto ChiakiNg::messages() noexcept -> messages::Messages& {
  return wiring_->messages;
}

auto ChiakiNg::play() noexcept -> play::Play& { return wiring_->play; }

auto ChiakiNg::kakusan()
    -> Expected<kakusan::Kakusan, kakusan::Kakusan::Unavailable> {
  return wiring_->kakusan();
}

auto ChiakiNg::use_stun_servers(std::span<const std::string> servers)
    -> Expected<void, kakusan::Kakusan::Unavailable> {
  return wiring_->use_stun_servers(servers);
}

auto ChiakiNg::caches()
    -> Expected<std::vector<catalog::CacheSummary>, catalog::CachesFailure> {
  return wiring_->catalog.caches();
}

auto ChiakiNg::cache_entries(catalog::CacheId cache)
    -> Expected<std::vector<catalog::CacheEntry>,
                catalog::CacheEntriesFailure> {
  return wiring_->catalog.cache_entries(cache);
}

auto ChiakiNg::clear_cache(catalog::CacheId cache)
    -> Expected<void, catalog::ClearCacheFailure> {
  return wiring_->catalog.clear_cache(cache);
}

auto ChiakiNg::clear_caches() -> Expected<void, catalog::ClearCachesFailure> {
  return wiring_->catalog.clear_caches();
}

auto ChiakiNg::http_calls() noexcept -> diagnostics::HttpCallLog& {
  return wiring_->http_calls;
}

auto ChiakiNg::sign_out(AccountId account)
    -> Expected<void, users::CredentialStore::Error> {
  wiring_->play.forget(account);
  wiring_->catalog.forget(account);
  wiring_->trophies.forget(account);
  return wiring_->users.sign_out(account);
}

}  // namespace libchiaking
