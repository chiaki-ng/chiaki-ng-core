/// @file
/// @brief Declares CloudLaunch::Core, the sequence that takes a cloud title
/// from the player's request to a reserved streaming server: device check,
/// catalog clearance, title preparation, datacenter choice and allocation.
#pragma once

#include "libchiaking/detail/catalog/launch_check.hpp"
#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/play/cloud_launch.hpp"
#include "libchiaking/detail/users/device_id.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/http/provider.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/runtime/utc_offset.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"
#include "src/core/play/adapters/cloud_launch/datacenter_launch_pick.hpp"
#include "src/core/play/adapters/cloud_launch/launch_failure.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/cloud_launch/title_clearance.hpp"
#include "src/core/play/adapters/datacenters/datacenters.hpp"
#include "src/core/play/adapters/links/cloud/server_slot.hpp"
#include "src/core/play/adapters/links/cloud/services/old_service_launch.hpp"
#include "src/core/play/adapters/links/cloud/services/ps5_cloud_launch.hpp"
#include "src/core/psn/gk/error.hpp"
#include "src/core/psn/gk/launch_spec.hpp"
#include "src/core/psn/server_clock.hpp"
#include "src/core/psn/types.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace libchiaking::diagnostics {
class Trace;
}

namespace libchiaking::play {

/// @brief The implementation behind the public CloudLaunch: one launch of one
/// title on the LegacyCloudStream or PS5 cloud service.
///
/// CloudLaunch::Core::run performs the whole launch on the session's thread:
/// it checks the request can be encoded, checks the cloud device with PS,
/// clears the title with the catalog (unless the start already did), prepares
/// the LegacyCloudStream title or takes the PS5 entitlement, and reserves a
/// server, measuring the datacenters and waiting for the player's datacenter
/// pick when asked to. A reservation PS refuses because the entitlement holds
/// no game is tried once more where the service allows it.
///
/// Progress, the datacenter offer and choice, the measurements and the
/// refusal are kept under a mutex, so the frontend's threads may read them and
/// submit a datacenter pick while the launch runs. The launch is over once
/// CloudLaunch::Core::run returns, after which picks are refused. The core is
/// owned by the cloud link's provisioning; the collaborators in Doors and the
/// Listener must outlive it. Neither copyable nor movable.
class CloudLaunch::Core final : private LaunchObserver {
 public:
  /// @brief One datacenter's measured round trips and path MTUs.
  using Ping = psn::gk::Ping;
  /// @brief A check made before anything is sent, which the request can fail.
  using Precondition = LaunchFailure::Precondition;

  /// @brief Receives what the launch does, for the cloud link to turn into
  /// stream events and log lines.
  ///
  /// Called on the launch's thread, except Listener::wake, which a submitting
  /// frontend thread calls.
  class Listener {
   public:
    /// @brief Allows destruction through the interface.
    virtual ~Listener() = default;

    /// @brief Told each time the launch's progress changes.
    ///
    /// @param progress The progress, with the time since the launch began.
    /// @param stage The stream failure stage the step belongs to.
    /// @param text The text shown to the player.
    virtual void progressed(const LaunchProgress& progress,
                            net::protocol::remote_play::FailureStage stage,
                            std::string_view text) = 0;
    /// @brief Told the rows to show once the datacenters are measured.
    ///
    /// @param picker_rows Every offered datacenter with its measurement.
    virtual void datacenters_measured(const std::vector<Ping>& picker_rows) = 0;
    /// @brief Told the offer when the player is asked to pick at launch.
    ///
    /// @param offer The datacenters offered and how long the player has.
    virtual void datacenters_offered(const DatacenterOffer& offer) = 0;
    /// @brief Told the datacenter the server is reserved in.
    ///
    /// @param choice How it was chosen; empty when its name is not a valid
    ///     DatacenterName.
    /// @param selected The selected datacenter's measurement.
    /// @param forced Whether it was forced or picked rather than chosen
    ///     automatically.
    /// @param reported_rtt_ms The round trip reported, in milliseconds: the
    ///     measured one, or for a forced pick the round trip measured before
    ///     the pick, 0 when there was none.
    virtual void datacenter_chosen(
        const std::optional<DatacenterChoice>& choice, const Ping& selected,
        bool forced, int reported_rtt_ms) = 0;
    /// @brief Idles the launch's thread while it waits for the player's pick.
    ///
    /// @param remaining The longest the thread may idle.
    virtual void idle(std::chrono::microseconds remaining) = 0;
    /// @brief Ends an idle early because a pick arrived; called from the
    /// submitting thread.
    virtual void wake() noexcept = 0;
    /// @brief Logs an informational line.
    ///
    /// @param message The line.
    virtual void note(std::string_view message) noexcept = 0;
  };

  /// @brief The providers and collaborators a launch uses, all borrowed; each
  /// must outlive the core.
  struct Doors {
    /// @brief Sends every request to PS.
    platform::HttpProvider& http;
    /// @brief Opens the sockets the datacenters are measured with.
    platform::NetworkProvider& network;
    /// @brief The monotonic clock progress and waits are timed by.
    platform::ClockProvider& clock;
    /// @brief Makes the launch's mutex and its datacenter pick's.
    platform::ThreadingProvider& threading;
    /// @brief Randomness for the measurement probes.
    platform::SecureRandomProvider& random;
    /// @brief Waits between allocation polls and on sockets.
    net::http::PollWaiter& waiter;
    /// @brief Clears the title and records PS's refusals.
    TitleClearance& clearance;
    /// @brief Supplies the device's UTC offset for the launch's time zone.
    const platform::UtcOffsetProvider& utc_offset;
    /// @brief Reads and records datacenter measurements; null to keep none.
    Datacenters* datacenters{};
    /// @brief Records the launch's diagnostic trace; may be null.
    const diagnostics::Trace* trace{};
    /// @brief PS's server time, used to stamp measurements; null to use the
    /// user's own.
    psn::ServerClock* server_clock{};
  };

  /// @brief What to launch and how. Plain data built from the cloud link
  /// configuration.
  struct Request {
    /// @brief The user to play as.
    std::shared_ptr<users::PsUser> user;
    /// @brief The title to launch.
    catalog::ProductId product;
    /// @brief Which datacenter to use, or how to choose one.
    DatacenterPreference datacenter{};
    /// @brief Prior measurements to use instead of the remembered ones; empty
    /// to use those.
    std::vector<Ping> prior{};
    /// @brief The time zone name sent with the launch.
    std::string time_zone{};
    /// @brief The picture height asked for, in lines.
    int resolution{};
    /// @brief The bitrate asked for, in kilobits per second; must be positive.
    int bitrate_kbps{};
    /// @brief Whether HDR is asked for.
    bool hdr{};
    /// @brief Whether to measure the bandwidth to the server.
    bool measure_bandwidth{};
    /// @brief A Takion version to use instead of the service's; 0 for none.
    std::uint32_t takion_version_override{};
    /// @brief The audio channels asked for on PS5 cloud.
    std::uint64_t requested_audio_channels{};
    /// @brief The widest audio layout PS5 cloud may send; 0 for its default.
    std::uint64_t max_audio_channels{};
    /// @brief Whether to skip the LegacyCloudStream account attributes check.
    bool skip_account_attributes_check{};
    /// @brief The player's screen reader and contrast choices for the cloud
    /// console.
    psn::gk::Accessibility accessibility{};
    /// @brief Whether PS5 cloud favours picture quality over steady
    /// performance.
    bool high_quality{true};
    /// @brief Cancels the launch.
    platform::StopToken stop{};
    /// @brief What the start already cleared; used only when it is for the
    /// same product.
    std::optional<catalog::ClearedLaunch> cleared{};
  };

  /// @brief Builds the launch, guessing the service from what is already known
  /// about the title.
  ///
  /// @param doors The providers and collaborators; each must outlive the core.
  /// @param request What to launch.
  /// @param listener Told what the launch does; must outlive the core.
  Core(Doors doors, Request request, Listener& listener);
  /// @brief Releases the launch.
  ~Core() override;

  /// @brief Names the PS streaming service of a service generation.
  ///
  /// @param service The generation.
  /// @return LegacyCloudStream for the old service, CloudStream otherwise.
  static auto streaming_service(catalog::ServiceGeneration service) noexcept
      -> psn::StreamingService;
  /// @brief Guesses which service will carry a title before the catalog
  /// clears it.
  ///
  /// Asks the clearance from held answers only, so it does not block.
  ///
  /// @param clearance The clearance asked; may be null.
  /// @param user The user; may be null.
  /// @param product The title.
  /// @return The service of a held cleared launch; otherwise LegacyCloudStream
  /// for a
  ///     product known to be for another platform than PS5, else PS5 cloud.
  static auto expected_service(TitleClearance* clearance, users::PsUser* user,
                               const catalog::ProductId& product)
      -> catalog::ServiceGeneration;

  /// @brief Reads the service the launch runs on.
  ///
  /// @return The guessed service until the catalog clears the title, then the
  ///     cleared one.
  auto service() const noexcept -> catalog::ServiceGeneration;
  /// @brief Reads the title.
  ///
  /// @return The product being launched; valid for the life of the core.
  auto product() const noexcept -> const catalog::ProductId& {
    return request_.product;
  }
  /// @brief Reads the latest progress.
  ///
  /// @return A copy of the progress.
  auto progress() const -> LaunchProgress;
  /// @brief Reads the datacenter offer.
  ///
  /// @return The offer once the datacenters are measured and the player is
  ///     asked to pick; empty otherwise.
  auto offer() const -> std::optional<DatacenterOffer>;
  /// @brief Submits the player's pick of a named datacenter.
  ///
  /// @param name The datacenter.
  /// @return As CloudLaunch::Core::choose_by_name.
  [[nodiscard]] auto choose_datacenter(const DatacenterName& name)
      -> Expected<void, DatacenterRefusal>;
  /// @brief Submits the player's wish for the lowest-latency datacenter.
  ///
  /// @return As CloudLaunch::Core::choose_by_name.
  [[nodiscard]] auto choose_lowest_latency()
      -> Expected<void, DatacenterRefusal>;
  /// @brief Submits a pick to the waiting launch and wakes it.
  ///
  /// May be called from any thread; does not block beyond a lock.
  ///
  /// @param name An offered name, or the automatic choice.
  /// @return Nothing when held; DatacenterRefusal::launch_over once the launch
  ///     ended; DatacenterRefusal::not_offered for a name not offered;
  ///     DatacenterRefusal::not_waiting when no pick is open.
  [[nodiscard]] auto choose_by_name(std::string_view name)
      -> Expected<void, DatacenterRefusal>;
  /// @brief Reads the datacenter choice.
  ///
  /// @return The choice once a server's datacenter is selected; empty before.
  auto chosen() const -> std::optional<DatacenterChoice>;
  /// @brief Reads this launch's measurements.
  ///
  /// @return The storable measurements; empty when none were taken or a
  ///     forced datacenter was not measured.
  auto measurements() const -> std::vector<DatacenterMeasurement>;
  /// @brief Reads why the launch was refused.
  ///
  /// @return The refusal once CloudLaunch::Core::run failed; empty otherwise.
  auto refusal() const -> std::optional<LaunchRefused>;

  /// @brief Checks what can be checked before anything is sent.
  ///
  /// @return The first unmet precondition: Precondition::invalid_spec for a
  ///     time zone containing a NUL or a request that cannot be encoded,
  ///     Precondition::invalid_npsso without a PSN account,
  ///     Precondition::invalid_bitrate for a bitrate that is not positive,
  ///     Precondition::ps5_cloud_launch_not_built when the title is known to
  ///     run on PS5 cloud streaming and this build leaves it out; empty when
  ///     the request is sound. A title whose id names no platform passes, as
  ///     it may belong to the older service; the launch stops once the catalog
  ///     clears it for PS5 cloud streaming.
  auto precondition() const -> std::optional<Precondition>;
  /// @brief Checks the user's cloud device with PS.
  ///
  /// Blocks on one request to PS.
  ///
  /// @param user The user's authorised calls.
  /// @param http The client the check is sent through.
  /// @param service The streaming service checked against.
  /// @return The device id; an identity-stage error when the device id could
  ///     not be made or the check failed.
  [[nodiscard]] static auto device_checked(users::PlayCalls& user,
                                           net::http::Client& http,
                                           psn::StreamingService service)
      -> Expected<users::DeviceId, psn::gk::Error>;
  /// @brief Checks the user's cloud device with PS through the launch's own
  /// HTTP provider; blocks on that request.
  ///
  /// @return Nothing; an identity-stage error when cancelled, without a user,
  ///     or when the check failed.
  [[nodiscard]] auto check_device() -> Expected<void, psn::gk::Error>;
  /// @brief Builds the launch specification sent with the allocation.
  ///
  /// @param entitlement The entitlement to launch.
  /// @param language The store language.
  /// @return The PS5 cloud or LegacyCloudStream specification with the
  /// service's Takion
  ///     version (or the override); Precondition::invalid_npsso without a PSN
  ///     account; Precondition::invalid_bitrate for a bitrate that is not
  ///     positive; Precondition::invalid_spec when it cannot be encoded.
  [[nodiscard]] auto launch_spec(std::string_view entitlement,
                                 std::string_view language) const
      -> Expected<psn::gk::LaunchSpec, Precondition>;
  /// @brief Runs the launch to a reserved server.
  ///
  /// Blocks the calling thread, normally the session's, on every request to
  /// PS, the datacenter measurements, the allocation queue and the player's
  /// datacenter pick; it returns when a server is reserved, the launch fails or
  /// the request's stop token fires. Call once.
  ///
  /// @return The reserved server; the failure otherwise, which is also kept
  ///     for CloudLaunch::Core::refusal.
  [[nodiscard]] auto run() -> Expected<ServerSlot, LaunchFailure>;

 private:
  /// @brief The service-specific half of the launch: none until cleared, then
  /// the LegacyCloudStream title steps or the PS5 entitlement.
  using Sequence =
      std::variant<std::monostate, OldServiceLaunch, Ps5CloudLaunch>;

  /// @brief What one reservation round launches.
  struct Target {
    /// @brief The entitlement id launched.
    std::string entitlement;
    /// @brief The platform the entitlement streams as.
    psn::StreamingPlatform platform{psn::StreamingPlatform::ps5};
  };

  /// @brief A device check failure paired with its trace code; defined in the
  /// implementation.
  struct TracedIdentity;

  /// @brief An opaque numeric code recorded in the diagnostic trace; it has no
  /// named values.
  enum class TraceCode : std::uint64_t {};

  /// @brief The pick that asks for the lowest-latency datacenter.
  static constexpr std::string_view automatic_choice =
      DatacenterLaunchPick::automatic;

  /// @brief Stamps progress with the time since the launch began, keeps it and
  /// tells the listener.
  ///
  /// @param progress The new progress.
  /// @param stage The stream failure stage.
  /// @param text The text shown to the player.
  void on_progress(const LaunchProgress& progress,
                   net::protocol::remote_play::FailureStage stage,
                   std::string_view text) override;
  /// @brief Keeps the measurements and, when the player is asked to pick,
  /// builds the offer; then tells the listener.
  ///
  /// @param picker_rows The rows shown to the player.
  /// @param measured_rows The measurements taken.
  void on_datacenters(const std::vector<Ping>& picker_rows,
                      const std::vector<Ping>& measured_rows) override;
  /// @brief Records the choice, saves fresh measurements and tells the
  /// listener.
  ///
  /// @param selected The selected datacenter.
  /// @param forced Whether it was forced or picked.
  void on_datacenter_selected(const Ping& selected, bool forced) override;
  /// @brief Finds a remembered bandwidth fit to reuse for the selected
  /// datacenter.
  ///
  /// @param selected The selected datacenter.
  /// @return The bandwidth; empty without a datacenter view, without a
  ///     measured round trip, or when none is fit.
  auto cached_bandwidth(const Ping& selected)
      -> std::optional<CachedBandwidth> override;
  /// @brief Records a measured bandwidth against the selected datacenter.
  ///
  /// @param selected The selected datacenter.
  /// @param kbps The bandwidth, in kilobits per second.
  void on_bandwidth_measured(const Ping& selected, int kbps) override;
  /// @brief Reports whether the player is asked to pick at launch.
  ///
  /// @return `true` when the datacenter preference asks at launch.
  auto selects_datacenter_at_launch() const noexcept -> bool override;
  /// @brief Opens the pick with the offered names.
  ///
  /// @param names The offered datacenter names.
  void offer_datacenters(const std::vector<std::string>& names) override;
  /// @brief Waits for the player's pick; blocks for up to the preference's
  /// wait.
  ///
  /// @param stop Cancels the wait.
  /// @return LaunchChoice::Kind::named or LaunchChoice::Kind::automatic for a
  ///     pick, LaunchChoice::Kind::timed_out when the time ran out,
  ///     LaunchChoice::Kind::cancelled when @p stop fired.
  auto await_datacenter_choice(const platform::StopToken& stop)
      -> LaunchChoice override;
  /// @brief Reads how long the player has to pick.
  ///
  /// @return The preference's wait.
  auto datacenter_choice_wait() const noexcept
      -> std::optional<std::chrono::milliseconds> override;

  /// @brief Performs the launch once; see CloudLaunch::Core::run.
  ///
  /// @param http The provider requests go through.
  /// @return The reserved server with its service, platform and entitlement;
  ///     an unmet precondition, a device check or reservation failure, a
  ///     catalog refusal, a LegacyCloudStream title failure, or a refusal by PS
  ///     when the entitlement holds no game.
  [[nodiscard]] auto attempt(platform::HttpProvider& http)
      -> Expected<ServerSlot, LaunchFailure>;
  /// @brief Checks the cloud device with PS; blocks on that request.
  ///
  /// @param http The provider the check goes through.
  /// @return Nothing; an identity-stage error when cancelled, without a user,
  ///     or when the check failed.
  [[nodiscard]] auto check_device(platform::HttpProvider& http)
      -> Expected<void, psn::gk::Error>;
  /// @brief Clears the title, reusing what the start cleared when it is for the
  /// same product; blocks on the catalog's requests otherwise.
  ///
  /// @param http Unused; the clearance has its own transport.
  /// @return The cleared launch; LaunchFailure::not_cleared with the catalog's
  ///     refusal, or with a malformed check when the catalog allowed but said
  ///     nothing to launch.
  [[nodiscard]] auto clear(platform::HttpProvider& http)
      -> Expected<catalog::ClearedLaunch, LaunchFailure>;
  /// @brief Finds the start's cleared launch when it is for the requested
  /// product, by id or by the id it is held as.
  ///
  /// @return The cleared launch, owned by the request; null when none applies.
  auto already_cleared() const -> const catalog::ClearedLaunch*;
  /// @brief Turns a catalog check into launch progress.
  ///
  /// @param check The check that started.
  void report(catalog::LaunchCheck check);
  /// @brief Reports whether the launch's current service is PS5 cloud
  /// streaming and this build leaves it out.
  ///
  /// @return `true` when the launch must stop before reaching PS.
  auto ps5_cloud_left_out() const noexcept -> bool;
  /// @brief Works out what a reservation round launches.
  ///
  /// For PS5 cloud this is the cleared entitlement; for LegacyCloudStream the
  /// title steps run, blocking on PS.
  ///
  /// @param cleared The cleared launch.
  /// @param round The round, from 0.
  /// @return The target; a LegacyCloudStream title failure.
  [[nodiscard]] auto target(const catalog::ClearedLaunch& cleared,
                            unsigned round) -> Expected<Target, LaunchFailure>;
  /// @brief Reports whether a reservation refused for having no game is worth a
  /// second round.
  ///
  /// @param cleared The cleared launch.
  /// @param round The round that failed.
  /// @return The service-specific answer.
  auto repeats_after_refusal(const catalog::ClearedLaunch& cleared,
                             unsigned round) const -> bool;
  /// @brief Opens a reservation, signs in, locks it and provisions a server;
  /// blocks on PS, the measurements, the pick and the queue.
  ///
  /// @param http The provider requests go through.
  /// @param spec The launch specification.
  /// @param platform The platform the entitlement streams as.
  /// @param prior Prior datacenter measurements.
  /// @return The reserved server; the reservation's error.
  [[nodiscard]] auto reserve(platform::HttpProvider& http,
                             psn::gk::LaunchSpec spec,
                             psn::StreamingPlatform platform,
                             const std::vector<Ping>& prior)
      -> Expected<ServerSlot, psn::gk::Error>;
  /// @brief Gives the prior measurements for the reservation.
  ///
  /// @return The request's own when given; otherwise the remembered ones;
  ///     empty without a datacenter view.
  auto prior_rows() const -> std::vector<Ping>;
  /// @brief Reads the forced datacenter name.
  ///
  /// @return The preference's name; empty when none is forced.
  auto forced_datacenter() const -> std::string_view;
  /// @brief Records how a pick was made and turns it into a launch choice.
  ///
  /// @param name The picked name.
  /// @return LaunchChoice::Kind::automatic for the automatic choice, otherwise
  ///     LaunchChoice::Kind::named with the name.
  auto resolved(std::string name) -> LaunchChoice;
  /// @brief Stamps a measurement with PS's time.
  ///
  /// @return PS's current time; the zero time when it is not known.
  auto measured_at() const -> DatacenterMeasurement::Time;
  /// @brief Gives PS's clock.
  ///
  /// @return The seam's clock, else the user's; null without either.
  auto ps_clock() const noexcept -> psn::ServerClock*;
  /// @brief Keeps the refusal of a failed launch and ends the launch.
  ///
  /// @param failure The failure.
  void remember(const LaunchFailure& failure);
  /// @brief Logs, at info level through Listener::note, a launch of a trial
  /// that failed after the catalog cleared it.
  ///
  /// The `[TRIAL]` line names the product, the failure, the HTTP status and,
  /// for a reservation, GK's stage, reason, detail code, the refusal
  /// remembered (if any) and PS's raw reply, so an unrecognised refusal can
  /// be mapped later. Nothing is logged once the launch was cancelled.
  ///
  /// @param failure The failure.
  void note_trial_refusal(const LaunchFailure& failure);

  /// @brief The providers and collaborators.
  Doors doors_;
  /// @brief What to launch.
  Request request_;
  /// @brief Told what the launch does.
  Listener& listener_;
  /// @brief The client the LegacyCloudStream title steps go through; built for
  /// them.
  std::optional<net::http::Client> title_http_;
  /// @brief The service-specific half of the launch.
  Sequence sequence_;
  /// @brief Guards the fields read from other threads.
  mutable platform::Mutex mutex_;
  /// @brief The service the launch runs on.
  catalog::ServiceGeneration service_;
  /// @brief The latest progress.
  LaunchProgress progress_;
  /// @brief The datacenter offer, when the player is asked to pick.
  std::optional<DatacenterOffer> offer_;
  /// @brief The datacenter choice, once made.
  std::optional<DatacenterChoice> chosen_;
  /// @brief The storable measurements of this launch.
  std::vector<DatacenterMeasurement> measurements_;
  /// @brief Why the launch was refused, once it failed.
  std::optional<LaunchRefused> refusal_;
  /// @brief The raw measurements of this launch.
  std::vector<Ping> measured_rows_;
  /// @brief The window for the player's datacenter pick.
  DatacenterLaunchPick pick_;
  /// @brief How the datacenter was chosen, once known from the pick.
  std::optional<DatacenterChoice::How> how_;
  /// @brief The measured round trip of a picked datacenter, in milliseconds.
  std::optional<int> picked_rtt_ms_;
  /// @brief Set when CloudLaunch::Core::run returned; picks are refused after.
  bool over_{};
  /// @brief When CloudLaunch::Core::run began, in monotonic microseconds.
  std::uint64_t began_us_{};
  /// @brief Whether the cleared copy is held as trial access; written and
  /// read only on the thread running CloudLaunch::Core::run.
  bool trial_{};
};

}  // namespace libchiaking::play
