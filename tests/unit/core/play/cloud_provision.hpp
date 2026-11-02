#pragma once

#include "libchiaking/platform/factory.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link.hpp"
#include "src/core/play/adapters/links/cloud/cloud_link_configuration.hpp"
#include "src/core/play/adapters/links/link.hpp"
#include "src/core/psn/gk/datacenter_ping.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace play_test {

class CloudProvision {
 public:
  using Configuration = libchiaking::play::CloudLinkConfiguration;
  using Allocation = libchiaking::play::CloudAllocation;
  using Failure = libchiaking::play::LinkFailure;
  using Result = libchiaking::Expected<Allocation, Failure>;

  static auto run(Configuration configuration,
                  libchiaking::platform::PlatformFactory& platform,
                  libchiaking::play::LinkEventSink* events = nullptr)
      -> Result {
    return run(std::move(configuration), platform,
               libchiaking::play::CloudLaunchSeams{}, events);
  }

  static auto run(Configuration configuration,
                  libchiaking::platform::PlatformFactory& platform,
                  libchiaking::play::CloudLaunchSeams seams,
                  libchiaking::play::LinkEventSink* events = nullptr)
      -> Result {
    namespace rp = libchiaking::net::protocol::remote_play;
    using Provisioning = libchiaking::play::CloudProvisioning;
    const auto service = Provisioning::expected_service(configuration, seams);
    const auto refused =
        [venue = Provisioning::venue_of(service)](Failure failure) {
          failure.reached_through(venue);
          return libchiaking::unexpected(std::move(failure));
        };
    if (!platform.supplies(libchiaking::platform::PlatformCapability::http |
                           libchiaking::platform::PlatformCapability::crypto))
      return refused(Failure{rp::FailureStage::cloud_provision,
                             rp::StreamError::provider_unavailable});
    if (auto audio = Failure::AudioRequestRefused::for_cloud(
            service, configuration.tuning.requested_audio_channels))
      return refused(Failure{rp::FailureStage::unknown,
                             rp::StreamError::invalid_configuration, *audio});
    libchiaking::platform::ClockProvider& clock(platform.clock());
    libchiaking::platform::SecureRandomProvider& random(platform.random());
    Progress observer{events, clock};
    Provisioning provisioning{configuration,
                              {platform.http(), platform.network(), clock,
                               platform.threading(), random},
                              seams,
                              observer,
                              Provisioning::DatacenterPick::never_at_launch};
    if (const auto invalid = provisioning.precondition())
      return refused(
          Failure{rp::FailureStage::cloud_provision,
                  rp::StreamError::invalid_configuration,
                  libchiaking::play::LaunchFailure::precondition(*invalid)});
    auto provisioned = provisioning.run();
    if (!provisioned) {
      auto failure = Failure::unlaunched(provisioned.error());
      if (configuration.stop.stop_requested()) failure.cancel();
      return refused(std::move(failure));
    }
    return Provisioning::allocation_of(std::move(provisioned.value()));
  }

 private:
  class Progress final : public libchiaking::play::CloudLaunch::Core::Listener {
   public:
    Progress(libchiaking::play::LinkEventSink* events,
             libchiaking::platform::ClockProvider& clock) noexcept
        : events_(events),
          clock_(clock),
          started_us_(clock.now_microseconds()) {}

    void progressed(const libchiaking::play::LaunchProgress&,
                    libchiaking::net::protocol::remote_play::FailureStage stage,
                    std::string_view text) override {
      namespace rp = libchiaking::net::protocol::remote_play;
      if (!events_) return;
      const auto now = clock_.now_microseconds();
      events_->on_event(rp::event::SetupProgress{
          stage, now >= started_us_ ? now - started_us_ : 0,
          std::string{text.substr(
              0, std::min(text.size(), rp::setup_progress_text_limit))}});
    }

    void datacenters_measured(
        const std::vector<libchiaking::psn::gk::Ping>& picker_rows) override {
      if (events_)
        events_->on_event(
            libchiaking::net::protocol::remote_play::event::CloudDatacenters{
                libchiaking::psn::gk::Ping::published(picker_rows)});
    }

    void datacenters_offered(
        const libchiaking::play::DatacenterOffer&) override {}

    void datacenter_chosen(
        const std::optional<libchiaking::play::DatacenterChoice>&,
        const libchiaking::psn::gk::Ping& selected, bool forced,
        int reported_rtt_ms) override {
      if (events_)
        events_->on_event(libchiaking::net::protocol::remote_play::event::
                              CloudDatacenterSelected{selected.name,
                                                      reported_rtt_ms, forced});
    }

    void idle(std::chrono::microseconds) override {}
    void wake() noexcept override {}
    void note(std::string_view) noexcept override {}

   private:
    libchiaking::play::LinkEventSink* events_;
    libchiaking::platform::ClockProvider& clock_;
    std::uint64_t started_us_;
  };
};

}  // namespace play_test
