/// @file
/// @brief Declares ServerReservation, one GK streaming session from its
/// start to an allocated server: sign-in, authorisation, lock, datacenter
/// choice, bandwidth and the allocation queue.
#pragma once

#include <libchiaking/detail/secret_string.hpp>
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"
#include "src/core/play/adapters/cloud_launch/launch_observer.hpp"
#include "src/core/play/adapters/datacenters/datacenter_ranking.hpp"
#include "src/core/play/adapters/links/cloud/server_slot.hpp"
#include "src/core/play/model/reservation_retry.hpp"
#include "src/core/psn/auth/authenticator.hpp"
#include "src/core/psn/gk/client.hpp"
#include "src/core/psn/gk/error.hpp"
#include "src/core/psn/gk/launch_spec.hpp"
#include "src/core/psn/gk/measurement.hpp"
#include "src/core/psn/types.hpp"
#include "src/core/users/cloud_relaunch.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::diagnostics {
class Trace;
}

namespace libchiaking::play {

/// @brief A GK session being reserved for one launch.
///
/// ServerReservation::open asks for client ids and the configuration and
/// starts the session; then the launch signs in, locks the session (waiting
/// while an old session closes), chooses a datacenter (measuring them, or
/// waiting for the player's pick), optionally measures bandwidth, and waits
/// in the allocation queue until a server is assigned. PS may rotate the
/// session's key on any reply; the newest is always used. Every step reports
/// numbered progress to the observer and is traced. Every request blocks on
/// PS; waits honour the client's stop token. Not synchronised; used on the
/// launching thread. The client, waiter and observer must outlive it.
class ServerReservation {
 public:
  /// @brief A server PS assigned.
  using Allocation = psn::gk::Allocation;
  /// @brief The GK client requests go through.
  using Client = psn::gk::Client;
  /// @brief The client ids of the service and platform.
  using ClientIds = psn::gk::ClientIds;
  /// @brief The session's rotating key.
  using ConfigKey = psn::gk::ConfigKey;
  /// @brief A datacenter as PS lists it.
  using Datacenter = psn::gk::Datacenter;
  /// @brief A GK failure, with its stage and reason.
  using Error = psn::gk::Error;
  /// @brief The launch specification.
  using LaunchSpec = psn::gk::LaunchSpec;
  /// @brief One datacenter's measurement.
  using Ping = psn::gk::Ping;
  /// @brief The GK session id.
  using ReservationId = psn::gk::SessionId;
  /// @brief The final datacenter choice.
  using Chosen = DatacenterRanking::Chosen;

  /// @brief Where the reservation waits and reports.
  struct Hooks {
    /// @brief Sleeps between polls; honours the stop token.
    net::http::Waiter& waiter;
    /// @brief Records the trace; may be null.
    const diagnostics::Trace* trace{};
    /// @brief Told progress and asked for the datacenter pick.
    LaunchObserver& observer = LaunchObserver::silent();
  };

  /// @brief The providers datacenter and bandwidth probes use, all borrowed.
  struct Probes {
    /// @brief Opens the probe sockets.
    platform::NetworkProvider& network;
    /// @brief The monotonic clock.
    platform::ClockProvider& clock;
    /// @brief Starts the resolver threads and makes their locks.
    platform::ThreadingProvider& threading;
    /// @brief Randomness for the probes.
    platform::SecureRandomProvider& random;
    /// @brief Waits on the probe sockets.
    net::http::PollWaiter& waiter;
  };

  /// @brief The number of numbered steps the progress counts.
  static constexpr int progress_steps = 10;
  /// @brief The most characters of a waiting progress text.
  static constexpr std::size_t progress_text_limit = 160;
  /// @brief The most characters of a datacenter progress text.
  static constexpr std::size_t datacenter_progress_text_limit = 96;

  /// @brief Measures a list of datacenters for a service with the session's
  /// lock key.
  using Survey = std::function<Expected<std::vector<Ping>, Error>(
      const std::vector<Datacenter>& datacenters, psn::StreamingService service,
      const SecretString& lock_key)>;

  /// @brief Opens a reservation: client ids, configuration and session start,
  /// steps 1 to 3. Blocks on three requests to PS.
  ///
  /// @param client The GK client; must outlive the reservation.
  /// @param hooks Where it waits and reports; must outlive it.
  /// @param spec The launch specification.
  /// @param platform The platform the title streams as.
  /// @return The reservation, keyed with the rotated key when PS sent one;
  ///     the step's error, or a cancelled error when the stop fired.
  [[nodiscard]] static auto open(Client& client, Hooks hooks, LaunchSpec spec,
                                 psn::StreamingPlatform platform)
      -> Expected<ServerReservation, Error>;
  /// @brief Waits until PS has had the settling time since the account's
  /// last cloud stream ended, before the launch asks it for anything.
  ///
  /// Reports the seconds left once, as a
  /// progress::WaitReason::recent_cloud_session wait while reserving, then
  /// waits them out. Returns at once when no stream ended within the
  /// settling time.
  ///
  /// @param client The GK client whose stop token cancels the wait.
  /// @param hooks Waits and is told the progress.
  /// @param service The service the launch runs on.
  /// @param relaunch The account's memory of its last cloud stream.
  /// @return Nothing once settled; a cancelled error when the stop fired.
  [[nodiscard]] static auto settle(Client& client, Hooks hooks,
                                   psn::StreamingService service,
                                   const users::CloudRelaunch& relaunch)
      -> Expected<void, Error>;
  /// @brief Opens a reservation, signs in and locks it, steps 1 to 7,
  /// starting a fresh session while PS refuses the lock outright.
  ///
  /// Each refused lock is reported as a progress::WaitReason::connection_retry
  /// wait, then a new session is opened, as ReservationRestart decides.
  /// Blocks on PS, the account's token requests and the waits.
  ///
  /// @param client The GK client; must outlive the reservation.
  /// @param hooks Where it waits and reports; must outlive it.
  /// @param spec The launch specification.
  /// @param platform The platform the title streams as.
  /// @param user The user's authorised calls.
  /// @param http The client the token requests go through.
  /// @return The locked reservation; the last step's error, or a cancelled
  ///     error when the stop fired.
  [[nodiscard]] static auto open_locked(Client& client, Hooks hooks,
                                        const LaunchSpec& spec,
                                        psn::StreamingPlatform platform,
                                        users::PlayCalls& user,
                                        net::http::Client& http)
      -> Expected<ServerReservation, Error>;
  /// @brief Turns an account authorisation refusal into a GK error.
  ///
  /// @param stage The stage it happened at.
  /// @param refusal The refusal.
  /// @return The authorisation's error, or an invalid npsso error when it had
  ///     none.
  [[nodiscard]] static auto refused(
      Error::Stage stage, const users::AuthorisationRefusal& refusal) noexcept
      -> Error;

  /// @brief Signs the user in for the session and authorises it, steps 4 to 6.
  /// Blocks on the account's token requests and on PS.
  ///
  /// A PlayStation Plus refusal is recorded on the user's membership.
  ///
  /// @param user The user's authorised calls.
  /// @param http The client the token requests go through.
  /// @return Nothing; the refusal or PS's error.
  [[nodiscard]] auto sign_in(users::PlayCalls& user, net::http::Client& http)
      -> Expected<void, Error>;
  /// @brief Authorises the session with codes the caller already has.
  ///
  /// @param codes The GK codes.
  /// @return Nothing; an invalid spec error when the codes cannot be applied;
  ///     PS's error.
  [[nodiscard]] auto authorize(const psn::auth::GkCodes& codes)
      -> Expected<void, Error>;
  /// @brief Locks the session, step 7, waiting while an old session closes.
  ///
  /// Polls up to 12 times at PS's interval, and retries a network error up
  /// to 5 times after 5 seconds. Blocks for all of it.
  ///
  /// @return Nothing once locked, which keeps the lock key; a cancelled error;
  ///     Reason::lock_unavailable when the polls run out; the request's error.
  [[nodiscard]] auto lock() -> Expected<void, Error>;
  /// @brief Fetches the datacenters PS offers; blocks on that request.
  ///
  /// @return The datacenters; a cancelled error; PS's error.
  [[nodiscard]] auto datacenters() -> Expected<std::vector<Datacenter>, Error>;
  /// @brief Tells PS the chosen datacenter and the measurements; blocks on
  /// that request.
  ///
  /// @param choice The choice.
  /// @return The selected datacenter with the port PS assigned; a cancelled
  ///     error; PS's error.
  [[nodiscard]] auto select(const Chosen& choice) -> Expected<Ping, Error>;
  /// @brief Chooses a datacenter, step 8 and 9, measuring with a given survey.
  ///
  /// Fetches the datacenters; measures them unless one is forced; when the
  /// player picks at launch, the forced name is ignored and the pick awaited.
  /// Blocks on PS, the survey and the pick.
  ///
  /// @param survey Measures the datacenters.
  /// @param forced The datacenter forced; empty or automatic for none.
  /// @param prior Remembered measurements.
  /// @return The choice, selected with PS; a cancelled error; the survey's
  ///     error; Reason::datacenter_unavailable for a forced or picked name not
  ///     offered; a no reachable datacenter error with the best round trip when
  ///     none qualifies; PS's error.
  [[nodiscard]] auto choose_datacenter(const Survey& survey,
                                       std::string_view forced,
                                       const std::vector<Ping>& prior)
      -> Expected<Chosen, Error>;
  /// @brief Chooses a datacenter, measuring with a DatacenterSurvey over the
  /// probes.
  ///
  /// @param probes The probes' providers.
  /// @param forced The datacenter forced; empty or automatic for none.
  /// @param prior Remembered measurements.
  /// @return As the other overload; a probe failure or cancellation from the
  ///     survey at the ping stage.
  [[nodiscard]] auto choose_datacenter(Probes probes, std::string_view forced,
                                       const std::vector<Ping>& prior)
      -> Expected<Chosen, Error>;
  /// @brief Waits in the allocation queue for a server, step 10.
  ///
  /// Polls at PS's frequency up to twice PS's first estimate (at most 900
  /// seconds, 300 when it gives none), and retries a network error up to 5
  /// times after 5 seconds. Blocks for all of it.
  ///
  /// @param selected The selected datacenter.
  /// @param bitrate_kbps The bitrate to allocate for, in kilobits per second.
  /// @return The allocation; a cancelled error; Reason::network_exhausted;
  ///     Reason::timed_out, or Reason::timed_out_unreachable when the last
  ///     poll failed; Reason::malformed for a reply neither queued nor
  ///     migrating; the request's error.
  [[nodiscard]] auto allocate(const Ping& selected, int bitrate_kbps)
      -> Expected<Allocation, Error>;
  /// @brief Chooses a datacenter, measures or reuses the bandwidth when asked,
  /// and allocates a server.
  ///
  /// @param probes The probes' providers.
  /// @param forced The datacenter forced; empty or automatic for none.
  /// @param prior Remembered measurements.
  /// @param bitrate_kbps The bitrate asked for, used when no bandwidth is
  ///     known.
  /// @param measure_up_to_kbps Measure bandwidth up to this rate; empty to
  ///     skip.
  /// @return The reserved server; the first step's error.
  [[nodiscard]] auto provision(Probes probes, std::string_view forced,
                               const std::vector<Ping>& prior, int bitrate_kbps,
                               std::optional<std::uint32_t> measure_up_to_kbps =
                                   std::nullopt) -> Expected<ServerSlot, Error>;
  /// @brief Measures the bandwidth to the selected datacenter; blocks for up
  /// to the survey window.
  ///
  /// @param probes The probes' providers.
  /// @param selected The selected datacenter.
  /// @param target_kbps The rate to test up to, in kilobits per second.
  /// @return The bandwidth in kilobits per second; empty without a lock key,
  ///     when stopped or when nothing was measured.
  [[nodiscard]] auto measure_bandwidth(Probes probes, const Ping& selected,
                                       std::uint32_t target_kbps)
      -> std::optional<int>;
  /// @brief Gives the bandwidth test rate for a picture height.
  ///
  /// @param height The picture height, in lines.
  /// @return 55000 above 1440, 45000 above 1080, otherwise 20000 kilobits per
  ///     second.
  static auto bandwidth_test_kbps(int height) noexcept -> std::uint32_t;

  /// @brief Reads the service.
  ///
  /// @return The launch specification's service.
  auto service() const noexcept -> psn::StreamingService {
    return spec_.service();
  }
  /// @brief Reads the client ids.
  ///
  /// @return The ids.
  auto client_ids() const noexcept -> const ClientIds& { return ids_; }
  /// @brief Reads the session id.
  ///
  /// @return The id.
  auto id() const noexcept -> const ReservationId& { return id_; }
  /// @brief Reads the current session key.
  ///
  /// @return The newest key.
  auto config_key() const noexcept -> const ConfigKey& { return key_; }
  /// @brief Reads the key in force when the lock was acquired.
  ///
  /// @return The key; empty before the lock.
  auto lock_key() const noexcept -> const std::optional<ConfigKey>& {
    return lock_key_;
  }
  /// @brief Reads the authorised launch specification.
  ///
  /// @return The specification; empty before authorising.
  auto authorized_spec() const noexcept -> const SecretString& {
    return authorized_;
  }

 private:
  /// @brief An error paired with its trace code; defined in the
  /// implementation.
  struct Traced;

  /// @brief An opaque numeric code recorded in the trace; it has no named
  /// values.
  enum class TraceCode : std::uint64_t {};

  /// @brief Says why a poll got no answer, in the retry policy's terms.
  ///
  /// @param error The poll's failure.
  /// @return cancelled, network for a transport failure, refused otherwise.
  static auto poll_failure_of(const Error& error) noexcept
      -> ReservationPollFailure;

  /// @brief The HTTP status PS refuses a session's lock with for good.
  static constexpr std::uint16_t lock_refused_status = 401;

  /// @brief Says whether PS refused to lock the session outright.
  ///
  /// @param error The lock's failure.
  /// @return True for a lock rejected with lock_refused_status.
  static auto refused_lock(const Error& error) noexcept -> bool;
  /// @brief Reports a refused lock and waits before a fresh session.
  ///
  /// @param client The GK client whose stop token cancels the wait.
  /// @param hooks Waits and is told the progress.
  /// @param service The service the launch runs on.
  /// @param restart The fresh session about to start, from 1.
  /// @param seconds The wait.
  /// @return False when the stop fired.
  static auto rest_before_restart(Client& client, Hooks hooks,
                                  psn::StreamingService service, int restart,
                                  int seconds) -> bool;

  /// @brief Builds an opened reservation.
  ///
  /// @param client The GK client.
  /// @param hooks Where it waits and reports.
  /// @param spec The launch specification.
  /// @param platform The platform.
  /// @param ids The client ids.
  /// @param id The session id.
  /// @param key The session key.
  ServerReservation(Client& client, Hooks hooks, LaunchSpec spec,
                    psn::StreamingPlatform platform, ClientIds ids,
                    ReservationId id, ConfigKey key) noexcept;

  /// @brief Names the service generation of a streaming service.
  ///
  /// @param service The service.
  /// @return The old service for LegacyCloudStream, PS5 cloud otherwise.
  static auto generation_of(psn::StreamingService service) noexcept
      -> catalog::ServiceGeneration;
  /// @brief Gives the trace code of a step's error.
  ///
  /// @param operation The step.
  /// @param error The error.
  /// @return A small code per kind of failure for authorise, lock and poll
  ///     steps; the error's own detail otherwise.
  static auto trace_code(diagnostics::Operation operation,
                         const Error& error) noexcept -> std::uint64_t;
  /// @brief Runs a step under the trace, recording its outcome and code.
  ///
  /// @tparam T The step's value type, or void.
  /// @tparam Step A callable returning Expected<T, Error>.
  /// @param trace The trace; may be null.
  /// @param operation The step.
  /// @param step The step.
  /// @return The step's result.
  template <class T, class Step>
  static auto observed(const diagnostics::Trace* trace,
                       diagnostics::Operation operation, Step&& step)
      -> Expected<T, Error>;
  /// @brief Finds the best measured round trip.
  ///
  /// @param rows The measurements.
  /// @return The lowest measured round trip in milliseconds; empty when none
  ///     was measured.
  static auto best_measured_rtt(const std::vector<Ping>& rows)
      -> std::optional<int>;

  /// @brief Applies the GK codes to the launch specification.
  ///
  /// @param codes The codes.
  /// @return The authorised specification; an invalid spec error.
  [[nodiscard]] auto authorized_from(const psn::auth::GkCodes& codes) const
      -> Expected<SecretString, Error>;
  /// @brief Keeps the authorised specification and authorises the session
  /// with it; blocks on PS.
  ///
  /// @param authorized The authorised specification.
  /// @return Nothing; a cancelled error; PS's error.
  [[nodiscard]] auto authorize_with(SecretString authorized)
      -> Expected<void, Error>;
  /// @brief Ranks the measurements, applies the player's pick when asked, and
  /// selects with PS.
  ///
  /// @param rows The offered datacenters.
  /// @param forced The datacenter forced.
  /// @param completed The measurements.
  /// @param prior Remembered measurements.
  /// @param pick_at_launch Whether to wait for the player's pick.
  /// @return As ServerReservation::choose_datacenter.
  [[nodiscard]] auto pick(const std::vector<Datacenter>& rows,
                          std::string_view forced, std::vector<Ping> completed,
                          const std::vector<Ping>& prior, bool pick_at_launch)
      -> Expected<Chosen, Error>;
  /// @brief Sleeps between polls.
  ///
  /// @param seconds How long.
  /// @return `true` after the full sleep; `false` when stopped.
  auto pause(int seconds) -> bool;
  /// @brief Takes a rotated key when PS sent one.
  ///
  /// @param rotated The key, if any.
  void rotate(std::optional<ConfigKey> rotated);
  /// @brief Reports a numbered step.
  ///
  /// @param activity What the launch does.
  /// @param stage The stream failure stage.
  /// @param label The step's label.
  /// @param step The step number.
  void announce(LaunchActivity activity,
                net::protocol::remote_play::FailureStage stage,
                std::string_view label, int step) const;
  /// @brief Reports a wait in the allocation queue or a data migration.
  ///
  /// @param waiting PS's queue or migration progress.
  /// @param attempt The poll, from 1.
  /// @param delay_seconds The wait before the next poll.
  void announce_waiting(const psn::gk::AllocationProgress& waiting, int attempt,
                        int delay_seconds) const;
  /// @brief Words the wait for the settling time.
  ///
  /// @param seconds The whole seconds left, rounded up.
  /// @return The text shown to the player.
  static auto settling_text(std::int64_t seconds) -> std::string;
  /// @brief Traces and reports a wait while locking.
  ///
  /// @param network Whether it waits after a network error.
  /// @param attempt The poll, from 1.
  /// @param delay_seconds The wait.
  /// @param event_name The old session's event PS named; may be empty.
  void announce_lock_wait(bool network, int attempt, int delay_seconds,
                          std::string_view event_name) const;
  /// @brief Traces and reports a wait while allocating.
  ///
  /// @param network Whether it waits after a network error.
  /// @param attempt The poll, from 1.
  /// @param delay_seconds The wait.
  /// @param waiting PS's progress; required unless @p network.
  void announce_allocation_wait(
      bool network, int attempt, int delay_seconds,
      const psn::gk::AllocationProgress* waiting) const;

  /// @brief The GK client.
  Client* client_;
  /// @brief Sleeps between polls.
  net::http::Waiter* waiter_;
  /// @brief Records the trace; may be null.
  const diagnostics::Trace* trace_;
  /// @brief Told progress and asked for the pick.
  LaunchObserver* observer_;
  /// @brief Phrases progress.
  LaunchReport report_;
  /// @brief The launch specification.
  LaunchSpec spec_;
  /// @brief The platform.
  psn::StreamingPlatform platform_;
  /// @brief The client ids.
  ClientIds ids_;
  /// @brief The session id.
  ReservationId id_;
  /// @brief The newest session key.
  ConfigKey key_;
  /// @brief The key in force when locked.
  std::optional<ConfigKey> lock_key_;
  /// @brief The authorised launch specification.
  SecretString authorized_;
};

}  // namespace libchiaking::play
