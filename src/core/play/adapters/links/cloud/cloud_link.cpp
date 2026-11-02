#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/ports.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/linked_stop.hpp"

#include "src/core/play/adapters/links/active_stream.hpp"
#include "src/core/play/adapters/links/session_diagnostics.hpp"
#include "src/core/utils/timing/deadline.hpp"

#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/net/protocol/remote_play/stream/active_session.hpp"
#include "src/core/net/protocol/remote_play/stream/controller_delivery.hpp"
#include "src/core/net/protocol/remote_play/stream/ctrl_features.hpp"
#include "src/core/net/protocol/remote_play/stream/public_media_event.hpp"
#include "src/core/net/protocol/remote_play/stream/stream_info.hpp"
#include "src/core/net/protocol/takion/platform_crypto_provider.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link.hpp"
#include "src/core/play/adapters/links/cloud/server_connect.hpp"
#include "src/core/play/adapters/session/seats_core.hpp"
#include "src/core/users/cloud_relaunch.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play {

namespace rp = net::protocol::remote_play;

class CloudLink final : public Link,
                        private ActiveStream::Owner,
                        private play::CloudLaunch::Core::Listener {
  struct ConnectFailure {
    play::ConnectError::Stage step;
  };

 public:
  CloudLink(CloudLinkConfiguration configuration,
            platform::PlatformFactory& platform,
            platform::CryptoProvider& crypto, platform::HttpProvider& http,
            CloudLaunchSeams seams)
      : Link(CloudProvisioning::venue_of(
            CloudProvisioning::expected_service(configuration, seams))),
        service_(CloudProvisioning::expected_service(configuration, seams)),
        configuration_(std::move(configuration)),
        platform_(platform),
        stream_(
            *this, ending(), platform, configuration_.stop,
            {.component = service_ == cloud::CloudService::legacy_cloud_stream
                              ? diagnostics::Component::legacy_cloud_stream
                              : diagnostics::Component::cloud,
             .console = Seats::Core::ConsoleKind::cloud,
             .feedback = configuration_.feedback,
             .has_ctrl = false,
             .cloud_receivers =
                 service_ == cloud::CloudService::cloud_stream
                     ? std::vector<
                           std::uint32_t>{CloudMessaging::game_intent_address,
                                          CloudMessaging::notification_address,
                                          CloudMessaging::banner_address,
                                          CloudMessaging::
                                              capture_result_address}
                     : std::vector<std::uint32_t>{}}),
        crypto_(crypto),
        http_(http),
        clock_(platform.clock()),
        random_(platform.random()),
        takion_crypto_(crypto, platform.threading()) {
    configuration_.stop =
        platform::StopToken{session_stop_ = std::make_shared<LinkedStop>(
                                std::move(configuration_.stop))};
    if (!configuration_.allocation)
      provisioning_.emplace(
          configuration_,
          CloudProvisioning::Providers{http_, platform_.network(), clock_,
                                       platform_.threading(), random_,
                                       &stream_.trace()},
          seams, static_cast<play::CloudLaunch::Core::Listener&>(*this));
  }

  auto launch_core() noexcept -> play::CloudLaunch::Core* {
    return provisioning_ ? provisioning_->core() : nullptr;
  }

  ~CloudLink() override { static_cast<void>(release_route()); }

  [[nodiscard]] auto start() -> Result override {
    if (stream_.state() != rp::StreamState::idle)
      return unexpected(rp::StreamError::invalid_state);
    if (!stream_.has_sink())
      return unexpected(rp::StreamError::invalid_configuration);
    if (auto audio = LinkFailure::AudioRequestRefused::for_cloud(
            service_, configuration_.tuning.requested_audio_channels)) {
      ending().record_failure(
          LinkFailure{rp::FailureStage::unknown,
                      rp::StreamError::invalid_configuration, *audio});
      return unexpected(rp::StreamError::invalid_configuration);
    }
    if (configuration_.allocation) return start_allocated();
    if (const auto invalid = provisioning_->precondition()) {
      ending().record_failure(
          LinkFailure{rp::FailureStage::cloud_provision,
                      rp::StreamError::invalid_configuration,
                      play::LaunchFailure::precondition(*invalid)});
      return unexpected(rp::StreamError::invalid_configuration);
    }
    if (configuration_.stop.stop_requested())
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::cancelled);
    if (!establish()) return {};
    stream_.at(rp::FailureStage::cloud_provision);
    auto provisioned =
        stream_.trace().observe_stage(rp::FailureStage::cloud_provision,
                                      [&] { return provisioning_->run(); });
    if (stream_.state() != rp::StreamState::establishing_route) return {};
    if (!provisioned)
      return stream_.fail(rp::StreamError::route_failed,
                          configuration_.stop.stop_requested()
                              ? rp::StreamFailure::cancelled
                              : rp::StreamFailure::route_establishment_failed,
                          LinkFailure::unlaunched(provisioned.error()));
    stream_.at(rp::FailureStage::active_prepare);
    streamed_platform_ = provisioned.value().platform;
    return connect(
        provisioned.value().allocation,
        play::Calibration::from_ping(
            provisioned.value().datacenter, provisioned.value().service,
            configuration_.tuning.takion_version_override));
  }

  [[nodiscard]] auto pump_once() -> Result override {
    const bool negotiated =
        stream_.state() == rp::StreamState::negotiating_stream ||
        stream_.streaming();
    if (!negotiated || !stream_.has_media())
      return unexpected(rp::StreamError::invalid_state);
    if (configuration_.stop.stop_requested()) return stop();
    stream_.observe();
    switch (stream_.pump_media()) {
      case ActiveStream::Progress::failed:
        return stream_.fail(rp::StreamError::pipeline_failed,
                            rp::StreamFailure::pipeline_failed);
      case ActiveStream::Progress::stopped:
        return stop();
      case ActiveStream::Progress::running:
        break;
    }
    if (const auto ready = stream_.stream_when_ready(std::nullopt); !ready)
      return stream_.fail(rp::StreamError::pipeline_failed,
                          rp::StreamFailure::pipeline_failed);
    stream_.keep_streaming();
    return {};
  }

  [[nodiscard]] auto select_datacenter(std::string_view name)
      -> Result override {
    auto* launch = launch_core();
    if (launch == nullptr) return unexpected(rp::StreamError::invalid_state);
    const auto chosen = launch->choose_by_name(name);
    if (chosen) return {};
    return unexpected(chosen.error() == play::DatacenterRefusal::not_offered
                          ? rp::StreamError::invalid_configuration
                          : rp::StreamError::invalid_state);
  }

  [[nodiscard]] auto stop() -> Result override {
    session_stop_->cancel();
    if (!stream_.stopping())
      return stream_.shutdown_failed()
                 ? unexpected(rp::StreamError::shutdown_failed)
                 : Result{};
    return stream_.stopped(release_route());
  }

  [[nodiscard]] auto send_create(CloudMessaging::CapturePress press)
      -> Result override {
    if (!configuration_.user)
      return unexpected(rp::StreamError::invalid_configuration);
    return stream_.send_cloud_message(
        CloudMessaging::capture_request_address, 1,
        CloudMessaging::capture_body(configuration_.user->account_id().value(),
                                     press));
  }

  [[nodiscard]] auto open_saved_data() -> Result override {
    return stream_.send_saved_data();
  }

  [[nodiscard]] auto toggle_system_menu(std::uint32_t pad) -> Result override {
    if (!streamed_platform_ ||
        !CloudProvisioning::has_system_menu(*streamed_platform_))
      return {};
    return stream_.send_system_menu_toggle(pad);
  }

  [[nodiscard]] auto connect_microphone() -> Result override {
    return stream_.send_microphone_connection();
  }

  [[nodiscard]] auto set_microphone_muted(bool muted) -> Result override {
    return stream_.send_microphone_mute(muted);
  }

  [[nodiscard]] auto submit_pin(std::span<const std::byte>) -> Result override {
    return unexpected(rp::StreamError::pin_rejected);
  }

  auto takion_version() const noexcept
      -> std::optional<std::uint32_t> override {
    if (const auto committed = stream_.committed_takion_version())
      return committed;
    const auto version = play::Calibration::takion_version(
        current_service(), configuration_.tuning.takion_version_override);
    if (version == 0) return std::nullopt;
    return version;
  }

 private:
  auto active_stream() noexcept -> ActiveStream* override { return &stream_; }
  auto active_stream() const noexcept -> const ActiveStream* override {
    return &stream_;
  }

  [[nodiscard]] auto release_route() -> bool override {
    const bool released = stream_.release_media();
    if (server_reached_ && configuration_.user)
      users::CloudRelaunch::of(*configuration_.user).ended();
    server_reached_ = false;
    return released;
  }

  auto establish() -> bool {
    started_ = timing::Deadline::now(platform_.clock());
    stream_.enter(rp::StreamState::establishing_route);
    return stream_.state() == rp::StreamState::establishing_route;
  }

  [[nodiscard]] auto connect(
      const psn::gk::Allocation& allocation,
      Expected<play::Calibration, play::ConnectError> calibration) -> Result {
    server_reached_ = true;
    auto active = stream_.trace().observe_stage(
        stream_.stage(),
        [&]() -> Expected<ServerConnect::Connected, ConnectFailure> {
          if (!calibration)
            return unexpected(ConnectFailure{calibration.error().stage()});
          auto connected =
              ServerConnect{{platform_.network(), clock_, platform_.threading(),
                             random_, crypto_, takion_crypto_}}
                  .connect(allocation, std::move(calibration.value()),
                           {configuration_.enable_dualsense_ctrl,
                            configuration_.enable_idr_on_fec_failure,
                            configuration_.tuning,
                            stream_.couch().session_pad_count()});
          if (!connected)
            return unexpected(ConnectFailure{connected.error().stage()});
          return std::move(connected.value());
        });
    if (!active) {
      const bool route =
          active.error().step == play::ConnectError::Stage::route;
      return stream_.fail(route ? rp::StreamError::route_failed
                                : rp::StreamError::pipeline_failed,
                          route ? rp::StreamFailure::route_establishment_failed
                                : rp::StreamFailure::negotiation_failed);
    }
    stream_.route_through(active.value().pollable);
    stream_.connect(std::move(active.value().session));
    stream_.enter(rp::StreamState::negotiating_stream);
    return {};
  }

  [[nodiscard]] auto start_allocated() -> Result {
    const auto& allocation = *configuration_.allocation;
    if (!valid_allocation(allocation)) {
      ending().record_failure(
          LinkFailure{rp::FailureStage::active_prepare,
                      rp::StreamError::invalid_configuration});
      return unexpected(rp::StreamError::invalid_configuration);
    }
    if (configuration_.stop.stop_requested())
      return stream_.fail(rp::StreamError::route_failed,
                          rp::StreamFailure::cancelled);
    auto calibration = play::Calibration::from_allocation(
        allocation.service, configuration_.tuning.takion_version_override,
        allocation.mtu_in, allocation.mtu_out, allocation.rtt_us);
    if (!calibration) {
      ending().record_failure(
          LinkFailure{rp::FailureStage::active_prepare,
                      rp::StreamError::invalid_configuration});
      return unexpected(rp::StreamError::invalid_configuration);
    }
    psn::gk::Allocation success;
    success.server_ip = allocation.server_ip;
    success.server_port = allocation.server_port;
    success.handshake_key.assign(allocation.handshake_key.view());
    success.launch_specification.assign(allocation.launch_spec.view());
    success.session_id.assign(allocation.session_id.view());
    success.psn_wrapper_type = allocation.psn_wrapper_type;
    if (!establish()) return {};
    stream_.at(rp::FailureStage::active_prepare);
    streamed_platform_ = allocation.platform;
    return connect(success, std::move(calibration));
  }

  static auto valid_allocation(const CloudAllocation& allocation) noexcept
      -> bool {
    return (allocation.service == cloud::CloudService::legacy_cloud_stream ||
            allocation.service == cloud::CloudService::cloud_stream) &&
           !allocation.server_ip.empty() && !allocation.launch_spec.empty() &&
           !allocation.handshake_key.empty();
  }

  auto current_service() const noexcept -> cloud::CloudService {
    if (configuration_.allocation) return configuration_.allocation->service;
    return provisioning_ ? provisioning_->service() : service_;
  }

  void progressed(const play::LaunchProgress&, rp::FailureStage stage,
                  std::string_view text) override {
    const auto elapsed =
        timing::elapsed(started_, timing::Deadline::now(platform_.clock()));
    stream_.emit(rp::event::SetupProgress{
        stage, elapsed.count(),
        std::string{text.substr(
            0, std::min(text.size(), rp::setup_progress_text_limit))}});
  }

  void datacenters_measured(
      const std::vector<psn::gk::Ping>& picker_rows) override {
    stream_.emit(
        rp::event::CloudDatacenters{psn::gk::Ping::published(picker_rows)});
  }

  void datacenters_offered(const play::DatacenterOffer&) override {}

  void datacenter_chosen(const std::optional<play::DatacenterChoice>&,
                         const psn::gk::Ping& selected, bool forced,
                         int reported_rtt_ms) override {
    stream_.emit(rp::event::CloudDatacenterSelected{selected.name,
                                                    reported_rtt_ms, forced});
  }

  void idle(std::chrono::microseconds remaining) override {
    stream_.idle(remaining);
  }

  void wake() noexcept override { stream_.wake(); }

  void note(std::string_view message) noexcept override {
    try {
      if (platform_.supplies(platform::PlatformCapability::diagnostics))
        platform_.diagnostics().write(
            {platform::LogSeverity::info, std::string{message}});
    } catch (...) {
    }
  }

  std::shared_ptr<LinkedStop> session_stop_;
  cloud::CloudService service_;
  CloudLinkConfiguration configuration_;
  platform::PlatformFactory& platform_;
  ActiveStream stream_;
  platform::CryptoProvider& crypto_;
  platform::HttpProvider& http_;
  platform::ClockProvider& clock_;
  platform::SecureRandomProvider& random_;
  net::protocol::takion::PlatformTakionCryptoProvider takion_crypto_;
  std::optional<CloudProvisioning> provisioning_;
  timing::Deadline::Instant started_{};
  bool server_reached_{};
  std::optional<cloud::CloudPlatform> streamed_platform_;
};

CloudProvisioning::CloudProvisioning(
    const CloudLinkConfiguration& configuration, Providers providers,
    CloudLaunchSeams seams, Core::Listener& listener, DatacenterPick pick)
    : clearance_(seams.clearance),
      sleep_waiter_(providers.threading, providers.clock),
      datacenters_(seams.datacenters) {
  if (clearance_ == nullptr && configuration.catalog != nullptr)
    clearance_ = &catalog_clearance_.emplace(*configuration.catalog);
  if (datacenters_ == nullptr && configuration.datacenters != nullptr)
    datacenters_ = &own_datacenters_.emplace(providers.threading,
                                             *configuration.datacenters);
  if (configuration.user == nullptr || !configuration.user->has_psn_account()) {
    unusable_ = Core::Precondition::invalid_npsso;
    return;
  }
  if (clearance_ == nullptr || !configuration.product) {
    unusable_ = Core::Precondition::invalid_spec;
    return;
  }
  Core::Request request{configuration.user, *configuration.product};
  request.datacenter = preference_of(configuration, pick);
  request.prior = psn::gk::Ping::from(configuration.prior_datacenters);
  request.time_zone = configuration.time_zone;
  request.resolution = configuration.resolution;
  request.bitrate_kbps = configuration.bitrate_kbps;
  request.hdr = configuration.hdr;
  request.measure_bandwidth = configuration.measure_bandwidth;
  request.takion_version_override =
      configuration.tuning.takion_version_override;
  request.requested_audio_channels =
      configuration.tuning.requested_audio_channels;
  request.max_audio_channels = configuration.tuning.max_audio_channels;
  request.skip_account_attributes_check =
      configuration.skip_account_attributes_check;
  request.accessibility = configuration.accessibility;
  request.high_quality = configuration.high_quality;
  request.stop = configuration.stop;
  request.cleared = configuration.cleared;
  core_ = std::make_unique<Core>(
      Core::Doors{providers.http, providers.network, providers.clock,
                  providers.threading, providers.random,
                  seams.waiter != nullptr
                      ? *seams.waiter
                      : static_cast<net::http::PollWaiter&>(sleep_waiter_),
                  *clearance_,
                  seams.utc_offset != nullptr
                      ? *seams.utc_offset
                      : static_cast<const platform::UtcOffsetProvider&>(
                            system_utc_offset_),
                  datacenters_, providers.trace, seams.server_clock},
      std::move(request), listener);
}

auto CloudProvisioning::preference_of(
    const CloudLinkConfiguration& configuration, DatacenterPick pick)
    -> play::DatacenterPreference {
  if (pick == DatacenterPick::never_at_launch &&
      configuration.datacenter.asks_at_launch())
    return play::DatacenterPreference{};
  return configuration.datacenter;
}

auto CloudProvisioning::expected_service(
    const CloudLinkConfiguration& configuration, const CloudLaunchSeams& seams)
    -> cloud::CloudService {
  if (configuration.allocation) return configuration.allocation->service;
  if (!configuration.product) return cloud::CloudService::cloud_stream;
  std::optional<play::CatalogClearance> catalog_clearance;
  auto* clearance = seams.clearance;
  if (clearance == nullptr && configuration.catalog != nullptr)
    clearance = &catalog_clearance.emplace(*configuration.catalog);
  return Core::streaming_service(Core::expected_service(
      clearance, configuration.user.get(), *configuration.product));
}

auto CloudProvisioning::service() const noexcept -> cloud::CloudService {
  return core_ ? Core::streaming_service(core_->service())
               : cloud::CloudService::cloud_stream;
}

auto CloudProvisioning::precondition() const
    -> std::optional<Core::Precondition> {
  if (unusable_) return unusable_;
  return core_->precondition();
}

auto CloudProvisioning::run()
    -> Expected<play::ServerSlot, play::LaunchFailure> {
  if (unusable_)
    return unexpected(play::LaunchFailure::precondition(*unusable_));
  return core_->run();
}

auto CloudProvisioning::allocation_of(play::ServerSlot slot)
    -> CloudAllocation {
  CloudAllocation allocation;
  allocation.service = slot.service;
  allocation.platform = slot.platform;
  allocation.entitlement_id = std::move(slot.entitlement_id);
  allocation.server_ip = slot.allocation.server_ip;
  allocation.server_port = slot.allocation.server_port;
  allocation.handshake_key.assign(slot.allocation.handshake_key.view());
  allocation.launch_spec.assign(slot.allocation.launch_specification.view());
  allocation.session_id.assign(slot.allocation.session_id.view());
  allocation.psn_wrapper_type = slot.allocation.psn_wrapper_type;
  const auto selected = slot.datacenter.reported();
  allocation.mtu_in =
      selected.mtu_in > 0 ? static_cast<std::uint32_t>(selected.mtu_in) : 0;
  allocation.mtu_out =
      selected.mtu_out > 0 ? static_cast<std::uint32_t>(selected.mtu_out) : 0;
  allocation.rtt_us = selected.rtt_ms > 0
                          ? static_cast<std::uint64_t>(selected.rtt_ms) *
                                microseconds_per_millisecond
                          : 0;
  allocation.datacenter_pings = psn::gk::Ping::published(slot.picker_rows);
  return allocation;
}

auto CloudLinkConfiguration::link(CloudLinkConfiguration configuration,
                                  platform::PlatformFactory& platform,
                                  CloudLaunchSeams seams)
    -> Expected<std::unique_ptr<Link>,
                net::protocol::remote_play::StreamError> {
  if (!platform.supplies(platform::PlatformCapability::crypto |
                         platform::PlatformCapability::http))
    return unexpected(
        net::protocol::remote_play::StreamError::provider_unavailable);
  return std::unique_ptr<Link>{new CloudLink(std::move(configuration), platform,
                                             platform.crypto(), platform.http(),
                                             seams)};
}

auto CloudLinkConfiguration::link(CloudLinkConfiguration configuration,
                                  platform::PlatformFactory& platform)
    -> Expected<std::unique_ptr<Link>,
                net::protocol::remote_play::StreamError> {
  return link(std::move(configuration), platform, CloudLaunchSeams{});
}

auto CloudProvisioning::venue_of(cloud::CloudService service) noexcept
    -> Venue {
  return Venue::cloud(service == cloud::CloudService::legacy_cloud_stream
                          ? catalog::ServiceGeneration::old_service
                          : catalog::ServiceGeneration::ps5_cloud);
}

auto CloudProvisioning::has_system_menu(cloud::CloudPlatform platform) noexcept
    -> bool {
  return platform == cloud::CloudPlatform::ps3;
}

auto CloudProvisioning::launch_of(Link& link) noexcept -> Core* {
  auto* cloud = dynamic_cast<CloudLink*>(&link);
  return cloud != nullptr ? cloud->launch_core() : nullptr;
}

}  // namespace libchiaking::play
