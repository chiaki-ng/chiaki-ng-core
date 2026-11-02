/// @file
/// @brief Declares DatacenterSurvey, which measures every datacenter a cloud
/// allocation offers in parallel with Senkusha probes, and the profiles,
/// targets and jobs it is built from.
#pragma once

#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/secret_string.hpp"
#include "libchiaking/platform/net/dns.hpp"
#include "libchiaking/platform/net/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/net/protocol/senkusha/senkusha.hpp"
#include "src/core/net/protocol/senkusha/types.hpp"
#include "src/core/net/protocol/takion/detail/envelope_codec.hpp"
#include "src/core/net/transport/datagram/transport.hpp"
#include "src/core/psn/gk/measurement.hpp"
#include "src/core/net/transport/http/poll_waiter.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace libchiaking::play {

/// @brief Which cloud service's probe format a survey speaks.
enum class PingService : std::uint8_t {
  /// @brief PS5 cloud: probes go out without an envelope.
  cloud_stream,
  /// @brief LegacyCloudStream: probes go out in the PSN envelope.
  legacy_cloud_stream,
};

/// @brief How one datacenter probe is shaped for a service: its envelope and
/// the Senkusha settings for round trip, MTU and optional bandwidth.
///
/// An immutable value.
class PingProfile {
 public:
  /// @brief Builds the profile.
  ///
  /// @param service The service.
  /// @param bandwidth How to measure bandwidth; empty to skip it.
  explicit PingProfile(
      PingService service,
      std::optional<net::protocol::senkusha::BandwidthPolicy>
          bandwidth = std::nullopt) noexcept
      : service_(service), bandwidth_(bandwidth) {}

  /// @brief Gives the envelope outgoing probes use.
  ///
  /// @return The PSN envelope for LegacyCloudStream; none for PS5 cloud.
  auto envelope() const noexcept -> net::protocol::takion::PacketEnvelope;
  /// @brief Builds the Senkusha client settings.
  ///
  /// @return Settings with the Senkusha Takion version, the fragmented big
  ///     echo, the reported round trip offset and floor, the default probe
  ///     count, the MTU bounds and timeouts, and the bandwidth policy.
  auto senkusha_config() const -> net::protocol::senkusha::ClientConfig;

 private:
  /// @brief The offset of the tag in each probe.
  static constexpr std::uint32_t probe_tag_offset = 3;
  /// @brief The shortest wait for an MTU probe's answer.
  static constexpr std::chrono::milliseconds mtu_timeout_floor{200};
  /// @brief The longest wait for an MTU probe's answer.
  static constexpr std::chrono::milliseconds mtu_timeout_ceiling{1000};

  /// @brief The service.
  PingService service_;
  /// @brief How to measure bandwidth; empty to skip it.
  std::optional<net::protocol::senkusha::BandwidthPolicy> bandwidth_;
};

/// @brief A datacenter's probe address: the first IPv4 address of its host,
/// at its port or the Senkusha port.
class PingTarget {
 public:
  /// @brief Why a datacenter has no probe address.
  enum class Error : std::uint8_t {
    /// @brief The datacenter has no host.
    invalid_host,
    /// @brief The port is past the largest port.
    invalid_port,
    /// @brief The host could not be resolved.
    dns_failure,
    /// @brief The host has no IPv4 address.
    no_ipv4_address,
  };

  /// @brief Resolves a datacenter's probe address; blocks on DNS.
  ///
  /// @param dns The resolver.
  /// @param datacenter The datacenter.
  /// @return The target; the reason it has none.
  [[nodiscard]] static auto resolve(platform::DnsProvider& dns,
                                    const psn::gk::Datacenter& datacenter)
      -> Expected<PingTarget, Error>;
  /// @brief Resolves a datacenter's probe address; blocks on DNS until it
  /// answers or @p stop fires.
  ///
  /// @param dns The resolver.
  /// @param datacenter The datacenter.
  /// @param stop Cancels the lookup.
  /// @return The target; the reason it has none.
  [[nodiscard]] static auto resolve(platform::DnsProvider& dns,
                                    const psn::gk::Datacenter& datacenter,
                                    const platform::StopToken& stop)
      -> Expected<PingTarget, Error>;

  /// @brief Reads the address.
  ///
  /// @return The endpoint.
  auto endpoint() const noexcept -> const platform::SocketEndpoint& {
    return endpoint_;
  }

 private:
  /// @brief Builds a target.
  ///
  /// @param endpoint The address.
  explicit PingTarget(platform::SocketEndpoint endpoint) noexcept
      : endpoint_(endpoint) {}

  /// @brief Resolves a datacenter's probe address.
  ///
  /// @param dns The resolver.
  /// @param datacenter The datacenter.
  /// @param stop Cancels the lookup; null for none.
  /// @return The target; Error::invalid_host, Error::invalid_port,
  ///     Error::dns_failure or Error::no_ipv4_address.
  [[nodiscard]] static auto lookup(platform::DnsProvider& dns,
                                   const psn::gk::Datacenter& datacenter,
                                   const platform::StopToken* stop)
      -> Expected<PingTarget, Error>;

  /// @brief The address.
  platform::SocketEndpoint endpoint_;
};

/// @brief One datacenter's measurement, polled until it finishes.
///
/// Used on the surveying thread. Not copyable.
class PingJob {
 public:
  /// @brief A measurement that did not finish.
  struct Failed {};
  /// @brief A finished measurement: the Senkusha result, or failed.
  using Finished = Expected<net::protocol::senkusha::Result, Failed>;

  /// @brief Builds the job.
  PingJob() = default;
  /// @brief Releases the job.
  virtual ~PingJob() = default;
  /// @brief Not copyable; a job owns its probe.
  PingJob(const PingJob&) = delete;
  /// @brief Not copy-assignable; a job owns its probe.
  auto operator=(const PingJob&) -> PingJob& = delete;

  /// @brief Builds a job that runs a Senkusha probe over a transport.
  ///
  /// @param transport The probe's transport; null makes the job fail.
  /// @param clock The monotonic clock; must outlive the job.
  /// @param threading Provider of the probe's locks; must outlive the job.
  /// @param random Randomness for the probe; must outlive the job.
  /// @param config The probe settings.
  /// @return The job.
  static auto over(
      std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
      platform::ClockProvider& clock, platform::ThreadingProvider& threading,
      platform::SecureRandomProvider& random,
      net::protocol::senkusha::ClientConfig config)
      -> std::unique_ptr<PingJob>;

  /// @brief Advances the measurement without blocking.
  ///
  /// @return The result once finished, and again on every later call; empty
  ///     while it runs.
  [[nodiscard]] virtual auto poll() -> std::optional<Finished> = 0;
  /// @brief Stops the measurement; it then finishes as failed.
  virtual void cancel() noexcept = 0;
  /// @brief Gives what to poll for the job's next answer.
  ///
  /// @return The pollable; null by default or when there is none.
  virtual auto pollable() const noexcept -> const platform::Pollable* {
    return nullptr;
  }

 protected:
  /// @brief The finished value of a failed measurement.
  ///
  /// @return The failure.
  static auto failed() -> Finished;
};

/// @brief The measurement of every datacenter of one allocation, run in
/// parallel within DatacenterSurvey::window_us.
///
/// Each datacenter gets a job: its host is resolved on a worker thread, then
/// a Senkusha probe measures round trip, MTU and optionally bandwidth. A
/// datacenter that fails or does not answer in time gets an unmeasured row.
/// Not synchronised; used on the launching thread. Move-only.
class DatacenterSurvey {
 public:
  /// @brief One job per datacenter.
  using Jobs = std::vector<std::unique_ptr<PingJob>>;

  /// @brief How long the survey may take, in microseconds.
  static constexpr std::uint64_t window_us = 15 * microseconds_per_second;

  /// @brief Why a survey did not start or finish.
  enum class Error : std::uint8_t {
    /// @brief The stop fired.
    cancelled,
    /// @brief No lock key was given.
    invalid_key,
    /// @brief The jobs do not match the datacenters one for one.
    count_mismatch,
    /// @brief A job is missing.
    null_job,
    /// @brief The deadline would overflow the clock.
    deadline_overflow,
  };

  /// @brief The providers the probes use, all borrowed.
  struct Providers {
    /// @brief Opens sockets and resolves hosts.
    platform::NetworkProvider& network;
    /// @brief The monotonic clock.
    platform::ClockProvider& clock;
    /// @brief Starts the resolver threads and makes their locks.
    platform::ThreadingProvider& threading;
    /// @brief Randomness for the probes.
    platform::SecureRandomProvider& random;
  };

  /// @brief What a survey found.
  struct Result {
    /// @brief One row per datacenter, in the order given.
    std::vector<psn::gk::Ping> rows;
    /// @brief Whether the window ran out before every datacenter answered.
    bool timed_out{};
  };

  /// @brief Starts measuring every datacenter.
  ///
  /// A datacenter without a host or with a bad port, or whose resolver thread
  /// cannot start, gets a job that fails at once.
  ///
  /// @param providers The providers; must outlive the survey.
  /// @param datacenters The datacenters PS offered.
  /// @param service The probe format.
  /// @param lock_key The allocation's lock key, which seals the big echo.
  /// @param stop Refuses to start when it already fired.
  /// @param bandwidth How to measure bandwidth; empty to skip it.
  /// @return The survey; Error::cancelled or Error::invalid_key.
  [[nodiscard]] static auto start(
      Providers providers, std::vector<psn::gk::Datacenter> datacenters,
      PingService service, const SecretString& lock_key,
      const platform::StopToken& stop,
      std::optional<net::protocol::senkusha::BandwidthPolicy> bandwidth =
          std::nullopt) -> Expected<DatacenterSurvey, Error>;
  /// @brief Starts a survey over jobs the caller made.
  ///
  /// @param clock The monotonic clock; must outlive the survey.
  /// @param datacenters The datacenters.
  /// @param jobs One job per datacenter.
  /// @return The survey; Error::count_mismatch, Error::null_job or
  ///     Error::deadline_overflow, after cancelling the jobs.
  [[nodiscard]] static auto start(const platform::ClockProvider& clock,
                                  std::vector<psn::gk::Datacenter> datacenters,
                                  Jobs jobs)
      -> Expected<DatacenterSurvey, Error>;

  /// @brief Moves a survey; the source is left finished.
  ///
  /// @param other The survey moved from.
  DatacenterSurvey(DatacenterSurvey&& other) noexcept;
  /// @brief Not copyable; the survey owns its jobs.
  DatacenterSurvey(const DatacenterSurvey&) = delete;
  /// @brief Not copy-assignable; the survey owns its jobs.
  auto operator=(const DatacenterSurvey&) -> DatacenterSurvey& = delete;
  /// @brief Not move-assignable.
  auto operator=(DatacenterSurvey&&) -> DatacenterSurvey& = delete;
  /// @brief Cancels the jobs still running.
  ~DatacenterSurvey();

  /// @brief Told how many datacenters have answered, out of all.
  using Measured = std::function<void(std::size_t done, std::size_t total)>;

  /// @brief Runs the survey to the end, blocking on the probes' sockets for up
  /// to DatacenterSurvey::window_us.
  ///
  /// @param waiter Waits on the jobs' sockets.
  /// @param stop Cancels the survey.
  /// @param measured Told each time the count of answered datacenters grows.
  /// @return The rows; Error::cancelled when @p stop fired or waiting failed.
  [[nodiscard]] auto run(net::http::PollWaiter& waiter,
                         const platform::StopToken& stop,
                         const Measured& measured = {})
      -> Expected<Result, Error>;
  /// @brief Counts the datacenters that answered.
  ///
  /// @return The count.
  auto reported() const noexcept -> std::size_t;
  /// @brief Polls every running job once without blocking.
  ///
  /// @return The rows once every job finished or the window passed; empty
  ///     while running or after the survey finished.
  auto step() -> std::optional<Result>;
  /// @brief Cancels the survey.
  ///
  /// @return The rows, unmeasured where no answer came; empty when already
  ///     finished.
  auto cancel() -> std::optional<std::vector<psn::gk::Ping>>;
  /// @brief Reads when the window ends.
  ///
  /// @return The deadline, in monotonic microseconds.
  auto deadline_us() const noexcept -> std::uint64_t { return deadline_us_; }

 private:
  /// @brief Builds the survey.
  ///
  /// @param clock The monotonic clock.
  /// @param datacenters The datacenters.
  /// @param jobs One job per datacenter.
  /// @param deadline_us When the window ends, in monotonic microseconds.
  DatacenterSurvey(const platform::ClockProvider& clock,
                   std::vector<psn::gk::Datacenter> datacenters, Jobs jobs,
                   std::uint64_t deadline_us);

  /// @brief Clamps a count into the range of `int`, from 0.
  ///
  /// @param value The count.
  /// @return The clamped value.
  static auto clamp_to_int(std::int64_t value) noexcept -> int;
  /// @brief Builds the row of a datacenter that did not answer.
  ///
  /// @param datacenter The datacenter.
  /// @return A row with the unmeasured round trip.
  static auto unmeasured_row(const psn::gk::Datacenter& datacenter)
      -> psn::gk::Ping;
  /// @brief Builds the row of a measured datacenter.
  ///
  /// @param datacenter The datacenter.
  /// @param result The Senkusha result.
  /// @return A measured row with the mean round trip in milliseconds, the
  ///     MTUs and the bandwidth when measured.
  static auto measured_row(
      const psn::gk::Datacenter& datacenter,
      const net::protocol::senkusha::Result& result) -> psn::gk::Ping;

  /// @brief Cancels every job still running.
  void cancel_active() noexcept;
  /// @brief Lists what to poll for the running jobs.
  ///
  /// @return The pollables; empty once finished.
  auto pollables() const -> std::vector<const platform::Pollable*>;
  /// @brief Reports whether every datacenter answered.
  ///
  /// @return `true` when every row is filled.
  auto all_reported() const noexcept -> bool;
  /// @brief Takes the rows, filling the missing ones as unmeasured, and
  /// finishes the survey.
  ///
  /// @return The rows.
  auto take_rows() -> std::vector<psn::gk::Ping>;

  /// @brief The monotonic clock.
  const platform::ClockProvider* clock_;
  /// @brief The datacenters, in order.
  std::vector<psn::gk::Datacenter> datacenters_;
  /// @brief One job per datacenter.
  Jobs jobs_;
  /// @brief Whether each job still runs.
  std::vector<bool> active_;
  /// @brief Each datacenter's row, once it answered.
  std::vector<std::optional<psn::gk::Ping>> rows_;
  /// @brief When the window ends, in monotonic microseconds.
  std::uint64_t deadline_us_;
  /// @brief Whether the rows were taken.
  bool finished_{};
};

}  // namespace libchiaking::play
