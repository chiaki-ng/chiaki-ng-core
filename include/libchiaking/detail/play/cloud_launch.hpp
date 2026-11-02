/// @file
/// @brief Declares the progress of a cloud launch and the handle a frontend
/// uses to follow and steer it.
#pragma once

#include "libchiaking/detail/catalog/platform.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/datacenter.hpp"
#include "libchiaking/detail/play/launch_refused.hpp"
#include "libchiaking/detail/progress.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

namespace libchiaking::play {

/// @brief Names the four broad phases of a cloud launch, in order.
///
/// The values run from 1 to 4, so they can drive a four-step indicator.
enum class LaunchPhase : std::uint8_t {
  /// @brief The launch has begun and has not reported an activity yet.
  starting = 1,
  /// @brief The title and the account are being checked and a session is
  /// being opened.
  preparing = 2,
  /// @brief A datacenter is being chosen and a server is being allocated.
  finding_server = 3,
  /// @brief The client is connecting to the allocated server.
  connecting = 4,
};

/// @brief Names what a cloud launch is doing right now.
///
/// The activities appear roughly in the order listed. Some belong to the older
/// cloud service only; see LaunchProgress::belongs_to.
enum class LaunchActivity : std::uint8_t {
  /// @brief Asking the catalog whether this account may stream the title.
  clearing_with_catalog,
  /// @brief Checking that this device may use cloud streaming.
  checking_device,
  /// @brief Opening a session for the title; older service only.
  opening_title_session,
  /// @brief Looking the title up on the service; older service only.
  resolving_title,
  /// @brief Taking the title's free licence; older service only.
  taking_free_licence,
  /// @brief Opening the account's session with the service; older service
  /// only.
  opening_account_session,
  /// @brief Asking the service to start a streaming session.
  reserving_server,
  /// @brief Measuring the round trip to each datacenter.
  measuring_datacenters,
  /// @brief Waiting for the player to answer a DatacenterOffer.
  waiting_for_datacenter_choice,
  /// @brief Waiting, possibly in a queue, for a server.
  allocating_server,
  /// @brief Waiting for the service to finish a data migration.
  migrating_data,
  /// @brief Connecting to the allocated server.
  connecting_to_server,
  /// @brief Measuring the connection to the chosen datacenter, or reusing a
  /// recent measurement, before asking for a server; PS5 cloud streaming
  /// with an automatic bitrate only.
  measuring_bandwidth,
  /// @brief Loading the user's games while the catalog clears the title;
  /// follows catalog::LaunchCheck::loading_library.
  loading_library,
  /// @brief Asking for the user's PlayStation Plus tier while the catalog
  /// clears the title; the catalog asks it only for the older service.
  /// Follows catalog::LaunchCheck::membership.
  checking_membership,
  /// @brief Reading the user's PS5 cloud streaming access fresh while the
  /// catalog clears the title; follows catalog::LaunchCheck::cloud_streaming.
  checking_cloud_streaming,
  /// @brief Asking PS again whether the user owns the title, because the
  /// held ownership is unknown; follows catalog::LaunchCheck::ownership.
  checking_ownership,
  /// @brief Re-reading the exact owned PS5 entitlement to confirm it still
  /// streams; follows catalog::LaunchCheck::entitlement.
  checking_entitlement,
  /// @brief Asking PS's selected-title resolver for the cloud version of
  /// the PS5 title; follows catalog::LaunchCheck::cloud_game.
  resolving_cloud_game,
  /// @brief Matching the PS5 title against the restriction list of excluded
  /// titles; follows catalog::LaunchCheck::exclusions.
  checking_exclusions,
  /// @brief Judging the release date against trusted time; follows
  /// catalog::LaunchCheck::release.
  checking_release,
};

/// @brief Describes where a cloud launch stands at one moment.
///
/// A LaunchProgress is an immutable snapshot. New ones are delivered through
/// SessionEvents::on_launch_progress as the launch advances, and the latest can
/// be read at any time from CloudLaunch::progress. Each optional count is
/// present only while the launch has that information.
class LaunchProgress {
 public:
  /// @brief A position of the form "number of total".
  using Step = progress::Position;

  /// @brief Optional figures that accompany an activity.
  struct Counts {
    /// @brief Position of the activity in the launch's list of steps;
    /// counted from 1.
    std::optional<Step> step{};
    /// @brief Place in the server queue; only with
    /// LaunchActivity::allocating_server.
    std::optional<std::uint32_t> queue_position{};
    /// @brief Estimated server queue wait, in seconds; only with
    /// LaunchActivity::allocating_server.
    std::optional<std::uint32_t> wait_time_estimate_seconds{};
    /// @brief Which try this is, counted from 1.
    std::optional<std::uint32_t> attempt{};
    /// @brief Percentage of data moved, 0 to 100; only with
    /// LaunchActivity::migrating_data.
    std::optional<std::uint32_t> migration_percent{};
    /// @brief Datacenters measured so far out of the total; only with
    /// LaunchActivity::measuring_datacenters.
    std::optional<Step> measured{};
    /// @brief Bandwidth the service is told, in kilobits per second, once
    /// known; only with LaunchActivity::measuring_bandwidth.
    std::optional<std::uint32_t> bandwidth_kbps{};
    /// @brief Age of a reused bandwidth measurement; only with
    /// Counts::bandwidth_kbps, and absent when the connection was just
    /// measured.
    std::optional<std::chrono::seconds> bandwidth_age{};
    /// @brief What the launch is waiting for, and for how long if known.
    std::optional<progress::Wait> wait{};
    /// @brief Time since the launch began.
    std::optional<std::chrono::microseconds> elapsed{};

    /// @brief Compares every figure.
    friend constexpr auto operator==(const Counts&, const Counts&) noexcept
        -> bool = default;
  };

  /// @brief Value of Counts::migration_percent when the migration is
  /// complete.
  static constexpr std::uint32_t whole_migration = 100;

  /// @brief Describes a launch that has begun and reported no activity yet.
  ///
  /// @param service Cloud service the launch is for.
  explicit constexpr LaunchProgress(catalog::ServiceGeneration service) noexcept
      : service_(service) {}

  /// @brief Describes a launch performing an activity, with no figures.
  ///
  /// @param service Cloud service the launch is for.
  /// @param activity What the launch is doing.
  /// @return The progress, or `std::nullopt` when `activity` does not belong
  ///     to `service`.
  [[nodiscard]] static constexpr auto at(catalog::ServiceGeneration service,
                                         LaunchActivity activity) noexcept
      -> std::optional<LaunchProgress> {
    return at(service, activity, Counts{});
  }

  /// @brief Describes a launch performing an activity, with figures.
  ///
  /// @param service Cloud service the launch is for.
  /// @param activity What the launch is doing.
  /// @param counts Figures to attach.
  /// @return The progress, or `std::nullopt` when `activity` does not belong
  ///     to `service`, a position or attempt is out of range, a figure is
  ///     attached to an activity it does not apply to, a bandwidth age has no
  ///     bandwidth, or the wait reason does not fit the activity
  ///     (LaunchProgress::waits_for).
  [[nodiscard]] static constexpr auto at(catalog::ServiceGeneration service,
                                         LaunchActivity activity,
                                         Counts counts) noexcept
      -> std::optional<LaunchProgress> {
    const bool step_valid =
        !counts.step ||
        (counts.step->number >= 1 && counts.step->number <= counts.step->of);
    const bool queue_valid =
        !counts.queue_position || activity == LaunchActivity::allocating_server;
    const bool wait_estimate_valid =
        !counts.wait_time_estimate_seconds ||
        (*counts.wait_time_estimate_seconds > 0 &&
         activity == LaunchActivity::allocating_server && counts.wait &&
         counts.wait->reason == progress::WaitReason::server_queue);
    const bool attempt_valid = !counts.attempt || *counts.attempt >= 1;
    const bool migration_valid = !counts.migration_percent ||
                                 (activity == LaunchActivity::migrating_data &&
                                  *counts.migration_percent <= whole_migration);
    const bool measured_valid =
        !counts.measured ||
        (activity == LaunchActivity::measuring_datacenters &&
         counts.measured->number <= counts.measured->of);
    const bool wait_valid =
        !counts.wait || waits_for(activity, counts.wait->reason);
    const bool bandwidth_valid =
        (!counts.bandwidth_kbps ||
         activity == LaunchActivity::measuring_bandwidth) &&
        (!counts.bandwidth_age || counts.bandwidth_kbps);
    if (!belongs_to(activity, service) || !step_valid || !queue_valid ||
        !attempt_valid || !migration_valid || !measured_valid || !wait_valid ||
        !wait_estimate_valid || !bandwidth_valid)
      return std::nullopt;
    LaunchProgress progress{service};
    progress.activity_ = activity;
    progress.counts_ = counts;
    return progress;
  }

  /// @brief Returns the phase an activity belongs to.
  ///
  /// @param activity Activity to place.
  /// @return The phase an activity belongs to.
  static constexpr auto phase_of(LaunchActivity activity) noexcept
      -> LaunchPhase {
    switch (activity) {
      case LaunchActivity::clearing_with_catalog:
      case LaunchActivity::loading_library:
      case LaunchActivity::checking_membership:
      case LaunchActivity::checking_cloud_streaming:
      case LaunchActivity::checking_ownership:
      case LaunchActivity::checking_entitlement:
      case LaunchActivity::resolving_cloud_game:
      case LaunchActivity::checking_exclusions:
      case LaunchActivity::checking_release:
      case LaunchActivity::checking_device:
      case LaunchActivity::opening_title_session:
      case LaunchActivity::resolving_title:
      case LaunchActivity::taking_free_licence:
      case LaunchActivity::opening_account_session:
      case LaunchActivity::reserving_server:
        return LaunchPhase::preparing;
      case LaunchActivity::measuring_datacenters:
      case LaunchActivity::measuring_bandwidth:
      case LaunchActivity::waiting_for_datacenter_choice:
      case LaunchActivity::allocating_server:
      case LaunchActivity::migrating_data:
        return LaunchPhase::finding_server;
      case LaunchActivity::connecting_to_server:
        break;
    }
    return LaunchPhase::connecting;
  }

  /// @brief Reports whether a service ever performs an activity.
  ///
  /// @param activity Activity to test.
  /// @param service Cloud service.
  /// @return `false` for the four activities only the older service performs
  ///     when `service` is PS5 cloud streaming; `true` otherwise.
  static constexpr auto belongs_to(LaunchActivity activity,
                                   catalog::ServiceGeneration service) noexcept
      -> bool {
    switch (activity) {
      case LaunchActivity::opening_title_session:
      case LaunchActivity::resolving_title:
      case LaunchActivity::taking_free_licence:
      case LaunchActivity::opening_account_session:
        return service == catalog::ServiceGeneration::old_service;
      default:
        break;
    }
    return true;
  }

  /// @brief Reports whether an activity can be waiting for a given reason.
  ///
  /// @param activity Activity to test.
  /// @param reason Reason for waiting.
  /// @return `true` when that reason can occur during that activity. The
  ///     console-related reasons never apply to a cloud launch; a connection
  ///     retry applies only while reserving, when a fresh session is started.
  static constexpr auto waits_for(LaunchActivity activity,
                                  progress::WaitReason reason) noexcept
      -> bool {
    switch (reason) {
      case progress::WaitReason::previous_session_closing:
      case progress::WaitReason::recent_cloud_session:
      case progress::WaitReason::title_not_ready:
      case progress::WaitReason::connection_retry:
        return activity == LaunchActivity::reserving_server;
      case progress::WaitReason::server_queue:
        return activity == LaunchActivity::allocating_server;
      case progress::WaitReason::data_migration:
        return activity == LaunchActivity::migrating_data;
      case progress::WaitReason::datacenter_choice:
        return activity == LaunchActivity::waiting_for_datacenter_choice;
      case progress::WaitReason::network_unreachable:
        return activity == LaunchActivity::reserving_server ||
               activity == LaunchActivity::allocating_server;
      case progress::WaitReason::console_waking:
      case progress::WaitReason::console_joining:
      case progress::WaitReason::console_answering:
        break;
    }
    return false;
  }

  /// @brief Returns a copy of this progress stamped with the time since the
  /// launch began.
  ///
  /// @param elapsed Time since the launch began.
  /// @return A copy of this progress stamped with the time since the launch
  ///     began.
  constexpr auto after(std::chrono::microseconds elapsed) const noexcept
      -> LaunchProgress {
    LaunchProgress stamped{*this};
    stamped.counts_.elapsed = elapsed;
    return stamped;
  }

  /// @brief Returns the cloud service the launch is for.
  ///
  /// @return The cloud service the launch is for.
  constexpr auto service() const noexcept -> catalog::ServiceGeneration {
    return service_;
  }
  /// @brief Returns the phase of the current activity, or
  /// LaunchPhase::starting when there is none yet.
  ///
  /// @return The phase of the current activity, or LaunchPhase::starting when
  ///     there is none yet.
  constexpr auto phase() const noexcept -> LaunchPhase {
    return activity_ ? phase_of(*activity_) : LaunchPhase::starting;
  }
  /// @brief Returns what the launch is doing, or `std::nullopt` before the
  /// first activity.
  ///
  /// @return What the launch is doing, or `std::nullopt` before the first
  ///     activity.
  constexpr auto activity() const noexcept -> std::optional<LaunchActivity> {
    return activity_;
  }
  /// @brief Returns the position of the activity in the launch's list of
  /// steps, when known.
  ///
  /// @return The position of the activity in the launch's list of steps, when
  ///     known.
  constexpr auto step() const noexcept -> std::optional<Step> {
    return counts_.step;
  }
  /// @brief Returns the player's place in the server queue, when the service
  /// reports one.
  ///
  /// @return The player's place in the server queue, when the service reports
  ///     one.
  constexpr auto queue_position() const noexcept
      -> std::optional<std::uint32_t> {
    return counts_.queue_position;
  }
  /// @brief Returns the estimated server queue wait, when the service reports
  /// one.
  ///
  /// @return The estimated server queue wait in seconds, when reported.
  constexpr auto wait_time_estimate_seconds() const noexcept
      -> std::optional<std::uint32_t> {
    return counts_.wait_time_estimate_seconds;
  }
  /// @brief Returns which try this is, when the activity is being repeated.
  ///
  /// @return Which try this is, when the activity is being repeated.
  constexpr auto attempt() const noexcept -> std::optional<std::uint32_t> {
    return counts_.attempt;
  }
  /// @brief Returns the percentage of data moved, when a migration is under
  /// way.
  ///
  /// @return The percentage of data moved, when a migration is under way.
  constexpr auto migration_percent() const noexcept
      -> std::optional<std::uint32_t> {
    return counts_.migration_percent;
  }
  /// @brief Returns how many datacenters have been measured out of the total,
  /// while measuring.
  ///
  /// @return How many datacenters have been measured out of the total, while
  ///     measuring.
  constexpr auto measured() const noexcept -> std::optional<Step> {
    return counts_.measured;
  }
  /// @brief Returns the bandwidth the service is told, in kilobits per
  /// second, once it is known.
  ///
  /// @return The bandwidth the service is told, in kilobits per second, once
  ///     it is known.
  constexpr auto bandwidth_kbps() const noexcept
      -> std::optional<std::uint32_t> {
    return counts_.bandwidth_kbps;
  }
  /// @brief Returns how old a reused bandwidth measurement is; absent when
  /// the connection was just measured.
  ///
  /// @return How old a reused bandwidth measurement is; absent when the
  ///     connection was just measured.
  constexpr auto bandwidth_age() const noexcept
      -> std::optional<std::chrono::seconds> {
    return counts_.bandwidth_age;
  }
  /// @brief Returns what the launch is waiting for, when it is waiting.
  ///
  /// @return What the launch is waiting for, when it is waiting.
  constexpr auto wait() const noexcept -> std::optional<progress::Wait> {
    return counts_.wait;
  }
  /// @brief Returns the time since the launch began, when it has been
  /// stamped.
  ///
  /// @return The time since the launch began, when it has been stamped.
  constexpr auto elapsed() const noexcept
      -> std::optional<std::chrono::microseconds> {
    return counts_.elapsed;
  }
  /// @brief Returns the figures in the form shared with console connection
  /// progress.
  ///
  /// @return A progress::Status with the step position, the attempt (1 when
  ///     unset), the elapsed time and the wait.
  constexpr auto status() const noexcept -> progress::Status {
    return {counts_.step, counts_.attempt.value_or(1), counts_.elapsed,
            counts_.wait};
  }

  /// @brief Compares service, activity and every figure.
  friend constexpr auto operator==(const LaunchProgress&,
                                   const LaunchProgress&) noexcept
      -> bool = default;

 private:
  catalog::ServiceGeneration service_;
  std::optional<LaunchActivity> activity_{};
  Counts counts_{};
};

/// @brief Names why a datacenter choice was not accepted.
enum class DatacenterRefusal : std::uint8_t {
  /// @brief The launch is not waiting for a choice: no offer is open.
  not_waiting,
  /// @brief The named datacenter is not in the current offer.
  not_offered,
  /// @brief The launch has already finished, successfully or not.
  launch_over,
};

/// @brief Lets a frontend follow one cloud launch and answer its datacenter
/// offer.
///
/// A cloud session owns its launch; the handle is obtained from Session::launch
/// and is valid for as long as that session. Every member may be called from
/// any thread, and the reading members return snapshots.
///
/// @see Play::start_on_cloud
/// @see SessionEvents::on_launch_progress
class CloudLaunch {
 public:
  /// @brief Implementation behind CloudLaunch; not part of the public
  /// surface.
  class Core;

  /// @brief Wraps a launch; called by the library when a cloud session is
  /// created.
  ///
  /// @param core Launch to expose. Borrowed; must outlive this handle.
  explicit CloudLaunch(Core& core) noexcept : core_(&core) {}
  /// @brief Not copyable; the session holds the one handle to its launch.
  CloudLaunch(const CloudLaunch&) = delete;
  /// @brief Not copy-assignable; the session holds the one handle to its
  /// launch.
  auto operator=(const CloudLaunch&) -> CloudLaunch& = delete;

  /// @brief Returns the cloud service the title is being launched on.
  ///
  /// Before the catalog has cleared the title this is the service expected
  /// from the title; it is corrected once the catalog answers.
  ///
  /// @return The cloud service the title is being launched on.
  auto service() const noexcept -> catalog::ServiceGeneration;
  /// @brief Returns the product being launched; valid for the life of the
  /// session.
  ///
  /// @return The product being launched; valid for the life of the session.
  auto product() const noexcept -> const catalog::ProductId&;
  /// @brief Returns the latest progress of the launch.
  ///
  /// @return The latest progress of the launch.
  auto progress() const -> LaunchProgress;
  /// @brief Returns the list of datacenters the player may pick from.
  ///
  /// @return The offer once datacenters have been measured, if the launch was
  ///     started with DatacenterPreference::ask_at_launch; `std::nullopt`
  ///     otherwise.
  /// @see SessionEvents::on_datacenters_offered
  auto offer() const -> std::optional<DatacenterOffer>;
  /// @brief Answers the open offer with one datacenter.
  ///
  /// The call only records the answer and wakes the launch. The result is
  /// reported through SessionEvents::on_datacenter_chosen as
  /// DatacenterChoice::How::picked_at_launch.
  ///
  /// @param name Datacenter to use; must be part of the offer.
  /// @return Nothing when the answer was recorded.
  /// @retval DatacenterRefusal::launch_over The launch has already finished.
  /// @retval DatacenterRefusal::not_waiting No offer is open.
  /// @retval DatacenterRefusal::not_offered `name` is not in the offer.
  [[nodiscard]] auto choose_datacenter(const DatacenterName& name)
      -> Expected<void, DatacenterRefusal>;
  /// @brief Answers the open offer by asking for the lowest-latency
  /// datacenter.
  ///
  /// The call only records the answer and wakes the launch. The result is
  /// reported through SessionEvents::on_datacenter_chosen as
  /// DatacenterChoice::How::lowest_at_launch.
  ///
  /// @return Nothing when the answer was recorded.
  /// @retval DatacenterRefusal::launch_over The launch has already finished.
  /// @retval DatacenterRefusal::not_waiting No offer is open.
  [[nodiscard]] auto choose_lowest_latency()
      -> Expected<void, DatacenterRefusal>;
  /// @brief Returns the datacenter the launch settled on, once it has.
  ///
  /// @return The datacenter the launch settled on, once it has.
  auto chosen() const -> std::optional<DatacenterChoice>;
  /// @brief Returns the latency measurements this launch took.
  ///
  /// @return One measurement per datacenter that answered; empty before
  ///     measuring, and for a launch forced onto a named datacenter.
  auto measurements() const -> std::vector<DatacenterMeasurement>;
  /// @brief Returns why the launch failed, once it has.
  ///
  /// @return The refusal after a failed launch; `std::nullopt` while the
  ///     launch runs and after it succeeds.
  /// @see Outcome::launch_refused
  auto refusal() const -> std::optional<LaunchRefused>;

 private:
  Core* core_;
};

}  // namespace libchiaking::play
