#include "src/core/play/play_core.hpp"
#include "src/core/play/usecases/nat_type_check.hpp"

#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"

#include <optional>
#include <utility>

namespace libchiaking::play {

Play::Core::Core(platform::PlatformFactory& platform,
                 consoles::KnownConsoles& consoles, catalog::Catalog& catalog,
                 DatacenterStore& datacenters, InternetPairer& pairer,
                 platform::DiagnosticsProvider* log)
    : Core(platform, consoles, catalog, datacenters, pairer, log,
           std::optional<StartCollaborators>{}) {}

Play::Core::Core(platform::PlatformFactory& platform,
                 consoles::KnownConsoles& consoles, catalog::Catalog& catalog,
                 DatacenterStore& datacenters, InternetPairer& pairer,
                 platform::DiagnosticsProvider* log,
                 StartCollaborators collaborators)
    : Core(platform, consoles, catalog, datacenters, pairer, log,
           std::optional<StartCollaborators>{std::move(collaborators)}) {}

Play::Core::Core(platform::PlatformFactory& platform,
                 consoles::KnownConsoles& consoles, catalog::Catalog& catalog,
                 DatacenterStore& datacenters, InternetPairer& pairer,
                 platform::DiagnosticsProvider* log,
                 std::optional<StartCollaborators> given)
    : log_(log != nullptr ? log
           : platform.supplies(platform::PlatformCapability::diagnostics)
               ? &platform.diagnostics()
               : nullptr),
      pairer_(pairer),
      datacenters_(platform.threading(), datacenters),
      live_(platform.threading(), platform.clock()),
      stun_(platform.network(), platform.clock(), platform.random()),
      nat_type_(stun_, log_),
      catalog_clearance_(catalog),
      launch_waiter_(platform.threading(), platform.clock()),
      start_(platform, consoles, catalog, datacenters, datacenters_, pairer,
             live_, log_,
             given ? std::move(*given) : own_collaborators()) {
  pairer_.core().watch_sessions([this] { return live_.playing(); });
}

auto Play::Core::own_collaborators() -> StartCollaborators {
  return StartCollaborators{catalog_clearance_, launch_waiter_, time_zone_,
                            Session::Core::core_settings()};
}

Play::Core::~Core() { pairer_.core().watch_sessions({}); }

Play::Play(platform::PlatformFactory& platform,
           consoles::KnownConsoles& consoles, catalog::Catalog& catalog,
           DatacenterStore& datacenters, InternetPairer& pairer,
           platform::DiagnosticsProvider* log)
    : core_(std::make_unique<Core>(platform, consoles, catalog, datacenters,
                                   pairer, log)) {}

Play::Play(std::unique_ptr<Core> core) noexcept : core_(std::move(core)) {}

Play::~Play() = default;

auto Play::start_on_console(std::shared_ptr<users::PsUser> player,
                            std::shared_ptr<consoles::Console> console,
                            Preferences preferences, media::MediaSink& media,
                            SessionEvents& events)
    -> Expected<std::shared_ptr<Session>, StartRefusal> {
  return core_->start().on_console(std::move(player), std::move(console),
                                   preferences, media, events);
}

auto Play::start_over_internet(std::shared_ptr<users::PsUser> player,
                               std::shared_ptr<consoles::Console> console,
                               Preferences preferences, media::MediaSink& media,
                               SessionEvents& events,
                               const platform::StopToken& stop)
    -> Expected<std::shared_ptr<Session>, StartRefusal> {
  return core_->start().over_internet(std::move(player), std::move(console),
                                      preferences, media, events, stop);
}

auto Play::start_on_cloud(std::shared_ptr<users::PsUser> player,
                          const catalog::ProductId& title,
                          Preferences preferences,
                          DatacenterPreference datacenter,
                          media::MediaSink& media, SessionEvents& events,
                          std::optional<catalog::ClearedLaunch> cleared)
    -> Expected<std::shared_ptr<Session>, StartRefusal> {
  return core_->start().on_cloud(std::move(player), title, preferences,
                                 datacenter, media, events, std::move(cleared));
}

auto Play::start_scripted(std::shared_ptr<users::PsUser> player, Where where,
                          testing::ScriptedLink& link, Preferences preferences,
                          media::MediaSink& media, SessionEvents& events)
    -> Expected<std::shared_ptr<Session>, StartRefusal> {
  return core_->start().scripted(std::move(player), where, link, preferences,
                                 media, events);
}

auto Play::starting() noexcept -> consoles::Starting& { return core_->start(); }

void Play::journal_sessions_in(
    std::shared_ptr<diagnostics::SessionJournal> journal) {
  core_->live().journal_in(std::move(journal));
}

auto Play::live() const -> std::shared_ptr<Session> {
  return core_->live().current();
}

auto Play::nat_type(const platform::StopToken& stop)
    -> Expected<NatType, NatTypeFailure> {
  return core_->nat_type().check(stop);
}

auto Play::known_datacenters(catalog::ServiceGeneration service) const
    -> KnownDatacenters {
  return core_->datacenters().known(service);
}

auto Play::forget_datacenters(catalog::ServiceGeneration service)
    -> Expected<void, DatacenterStore::Error> {
  return core_->datacenters().forget(service);
}

void Play::forget(AccountId account) { core_->live().leave_as(account); }

}  // namespace libchiaking::play
