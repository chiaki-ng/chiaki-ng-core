#include "libchiaking/detail/play/cloud_launch.hpp"

#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"

#include <string_view>
#include <utility>

namespace libchiaking::play {


auto CloudLaunch::service() const noexcept -> catalog::ServiceGeneration {
  return core_->service();
}

auto CloudLaunch::product() const noexcept -> const catalog::ProductId& {
  return core_->product();
}

auto CloudLaunch::progress() const -> LaunchProgress {
  return core_->progress();
}

auto CloudLaunch::offer() const -> std::optional<DatacenterOffer> {
  return core_->offer();
}

auto CloudLaunch::choose_datacenter(const DatacenterName& name)
    -> Expected<void, DatacenterRefusal> {
  return core_->choose_datacenter(name);
}

auto CloudLaunch::choose_lowest_latency() -> Expected<void, DatacenterRefusal> {
  return core_->choose_lowest_latency();
}

auto CloudLaunch::chosen() const -> std::optional<DatacenterChoice> {
  return core_->chosen();
}

auto CloudLaunch::measurements() const -> std::vector<DatacenterMeasurement> {
  return core_->measurements();
}

auto CloudLaunch::refusal() const -> std::optional<LaunchRefused> {
  return core_->refusal();
}


auto CloudLaunch::Core::streaming_service(
    catalog::ServiceGeneration service) noexcept -> psn::StreamingService {
  return service == catalog::ServiceGeneration::old_service
             ? psn::StreamingService::legacy_cloud_stream
             : psn::StreamingService::cloud_stream;
}

auto CloudLaunch::Core::expected_service(TitleClearance* clearance,
                                         users::PsUser* user,
                                         const catalog::ProductId& product)
    -> catalog::ServiceGeneration {
  if (clearance != nullptr && user != nullptr && user->has_psn_account()) {
    const auto held = clearance->held(*user, product);
    if (const auto& cleared = held.cleared_launch()) return cleared->service;
  }
  const auto platform = product.platform();
  return platform && *platform != catalog::Platform::ps5
             ? catalog::ServiceGeneration::old_service
             : catalog::ServiceGeneration::ps5_cloud;
}

auto CloudLaunch::Core::service() const noexcept -> catalog::ServiceGeneration {
  const platform::Lock lock{mutex_};
  return service_;
}

auto CloudLaunch::Core::progress() const -> LaunchProgress {
  const platform::Lock lock{mutex_};
  return progress_;
}

auto CloudLaunch::Core::offer() const -> std::optional<DatacenterOffer> {
  const platform::Lock lock{mutex_};
  return offer_;
}

auto CloudLaunch::Core::chosen() const -> std::optional<DatacenterChoice> {
  const platform::Lock lock{mutex_};
  return chosen_;
}

auto CloudLaunch::Core::measurements() const
    -> std::vector<DatacenterMeasurement> {
  const platform::Lock lock{mutex_};
  return measurements_;
}

auto CloudLaunch::Core::refusal() const -> std::optional<LaunchRefused> {
  const platform::Lock lock{mutex_};
  return refusal_;
}

auto CloudLaunch::Core::choose_datacenter(const DatacenterName& name)
    -> Expected<void, DatacenterRefusal> {
  return choose_by_name(name.text());
}

auto CloudLaunch::Core::choose_lowest_latency()
    -> Expected<void, DatacenterRefusal> {
  return choose_by_name(automatic_choice);
}

auto CloudLaunch::Core::choose_by_name(std::string_view name)
    -> Expected<void, DatacenterRefusal> {
  {
    const platform::Lock lock{mutex_};
    if (over_) return unexpected(DatacenterRefusal::launch_over);
  }
  switch (pick_.submit(name)) {
    case DatacenterLaunchPick::Submit::accepted:
      break;
    case DatacenterLaunchPick::Submit::not_offered:
      return unexpected(DatacenterRefusal::not_offered);
    case DatacenterLaunchPick::Submit::not_waiting:
      return unexpected(DatacenterRefusal::not_waiting);
  }
  listener_.wake();
  return {};
}

}  // namespace libchiaking::play
