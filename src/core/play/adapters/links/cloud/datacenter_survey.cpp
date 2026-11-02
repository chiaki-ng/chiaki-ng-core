#include "src/core/play/adapters/links/cloud/datacenter_survey.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"

#include "src/core/net/ports.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/net/transport/datagram/udp_transport.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>

namespace libchiaking::play {

auto DatacenterSurvey::clamp_to_int(std::int64_t value) noexcept -> int {
  return static_cast<int>(
      std::clamp<std::int64_t>(value, 0, std::numeric_limits<int>::max()));
}

auto DatacenterSurvey::unmeasured_row(const psn::gk::Datacenter& datacenter)
    -> psn::gk::Ping {
  auto row = psn::gk::Ping::from(datacenter);
  row.rtt_ms = psn::gk::Ping::unmeasured_rtt_ms;
  row.rtts_ms = {psn::gk::Ping::unmeasured_rtt_ms};
  return row;
}

auto DatacenterSurvey::measured_row(
    const psn::gk::Datacenter& datacenter,
    const net::protocol::senkusha::Result& result) -> psn::gk::Ping {
  auto row = psn::gk::Ping::from(datacenter);
  row.measured = true;
  row.rtt_ms =
      clamp_to_int(std::chrono::duration_cast<std::chrono::milliseconds>(
                       result.round_trip_time.raw_mean)
                       .count());
  row.rtts_ms = {row.rtt_ms};
  row.mtu_in = clamp_to_int(result.inbound_mtu);
  row.mtu_out = clamp_to_int(result.outbound_mtu);
  if (result.bandwidth)
    row.bandwidth_kbps = clamp_to_int(result.bandwidth->kbps);
  return row;
}

auto PingJob::failed() -> Finished { return unexpected(PingJob::Failed{}); }

class SenkushaJob final : public PingJob {
 public:
  SenkushaJob(
      std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
      platform::ClockProvider& clock, platform::ThreadingProvider& threading,
      platform::SecureRandomProvider& random,
      net::protocol::senkusha::ClientConfig config)
      : pollable_(transport ? transport->pollable() : nullptr),
        client_(std::make_unique<net::protocol::senkusha::Client>(
            std::move(transport), clock, threading, random, sink_,
            std::move(config))) {}

  auto poll() -> std::optional<Finished> override {
    if (finished_) return finished_;
    if (!started_) {
      started_ = true;
      if (!client_->start()) return fail();
    }
    if (!client_->pump_once()) return fail();
    auto snapshot = client_->snapshot();
    if (snapshot.state == net::protocol::senkusha::State::completed &&
        snapshot.result) {
      finished_.emplace(std::move(*snapshot.result));
      client_.reset();
      return finished_;
    }
    if (snapshot.state == net::protocol::senkusha::State::failed ||
        snapshot.error)
      return fail();
    return std::nullopt;
  }

  void cancel() noexcept override {
    if (finished_) return;
    try {
      static_cast<void>(client_->cancel());
    } catch (...) {
    }
    client_.reset();
    finished_.emplace(failed());
  }

  auto pollable() const noexcept -> const platform::Pollable* override {
    return finished_ || !client_ ? nullptr : pollable_;
  }

 private:
  class Sink final : public net::protocol::senkusha::EventSink {
   public:
    void on_event(net::protocol::senkusha::Event) override {}
  };

  auto fail() -> std::optional<Finished> {
    finished_.emplace(failed());
    client_.reset();
    return finished_;
  }

  Sink sink_;
  const platform::Pollable* pollable_{};
  std::unique_ptr<net::protocol::senkusha::Client> client_;
  bool started_{};
  std::optional<Finished> finished_;
};

class FailedJob final : public PingJob {
 public:
  auto poll() -> std::optional<Finished> override { return failed(); }
  void cancel() noexcept override {}
};

class Resolution {
 public:
  [[nodiscard]] static auto start(platform::ThreadingProvider& threading,
                                  platform::DnsProvider& dns,
                                  psn::gk::Datacenter datacenter)
      -> std::unique_ptr<Resolution> {
    auto state = std::make_shared<State>(threading);
    auto worker = platform::Thread::start(
        threading, "datacenter resolver", platform::ThreadRole::unspecified,
        [state, &dns, datacenter = std::move(datacenter)] {
          const platform::StopToken stop{state};
          const auto target = PingTarget::resolve(dns, datacenter, stop);
          const platform::Lock lock(state->mutex);
          if (target) state->endpoint = target.value().endpoint();
          state->done = true;
        });
    if (!worker) return nullptr;
    return std::unique_ptr<Resolution>{
        new Resolution{std::move(state), std::move(worker).value()}};
  }

  Resolution(const Resolution&) = delete;
  auto operator=(const Resolution&) -> Resolution& = delete;

  ~Resolution() {
    abandon();
    if (worker_.joinable()) static_cast<void>(worker_.join());
  }

  void abandon() noexcept { state_->stopped.store(true); }
  auto abandoned() const noexcept -> bool { return state_->stopped.load(); }

  auto outcome()
      -> std::optional<Expected<platform::SocketEndpoint, PingJob::Failed>> {
    std::optional<platform::SocketEndpoint> endpoint;
    {
      const platform::Lock lock(state_->mutex);
      if (!state_->done) return std::nullopt;
      endpoint = state_->endpoint;
    }
    if (worker_.joinable()) static_cast<void>(worker_.join());
    if (!endpoint)
      return Expected<platform::SocketEndpoint, PingJob::Failed>{
          unexpected(PingJob::Failed{})};
    return Expected<platform::SocketEndpoint, PingJob::Failed>{*endpoint};
  }

 private:
  struct State final : platform::StopState {
    explicit State(platform::ThreadingProvider& threading) : mutex(threading) {}

    bool stop_requested() const noexcept override { return stopped.load(); }

    std::atomic<bool> stopped{false};
    platform::Mutex mutex;
    bool done{};
    std::optional<platform::SocketEndpoint> endpoint;
  };

  Resolution(std::shared_ptr<State> state, platform::Thread worker) noexcept
      : state_(std::move(state)), worker_(std::move(worker)) {}

  std::shared_ptr<State> state_;
  platform::Thread worker_;
};

class ResolvingJob final : public PingJob {
 public:
  ResolvingJob(DatacenterSurvey::Providers providers,
               std::unique_ptr<Resolution> resolution, PingProfile profile,
               std::shared_ptr<const SecretString> lock_key)
      : providers_(providers),
        profile_(profile),
        lock_key_(std::move(lock_key)),
        resolution_(std::move(resolution)) {}

  ~ResolvingJob() override { cancel(); }

  auto poll() -> std::optional<Finished> override {
    if (probe_) return probe_->poll();
    if (failed_ || resolution_->abandoned()) return failed();
    const auto endpoint = resolution_->outcome();
    if (!endpoint) return std::nullopt;
    if (*endpoint) {
      if (auto probe = start_probe(endpoint->value())) {
        probe_ = std::move(probe.value());
        return probe_->poll();
      }
    }
    failed_ = true;
    return failed();
  }

  void cancel() noexcept override {
    resolution_->abandon();
    if (probe_) probe_->cancel();
  }

  auto pollable() const noexcept -> const platform::Pollable* override {
    return probe_ ? probe_->pollable() : nullptr;
  }

 private:
  auto start_probe(const platform::SocketEndpoint& endpoint)
      -> Expected<std::unique_ptr<PingJob>, Failed> {
    net::transport::datagram::UdpTransportConfig udp;
    udp.local = {platform::IpAddress::any_ipv4(), 0};
    udp.peer = endpoint;
    udp.dont_fragment = false;
    auto transport = net::transport::datagram::ProviderUdpTransport::create(
        providers_.network, udp);
    if (!transport) return unexpected(Failed{});
    auto config = profile_.senkusha_config();
    config.profile.big.session_key =
        net::protocol::takion::SharedSecretText{lock_key_};
    return PingJob::over(std::move(transport.value()), providers_.clock,
                         providers_.threading, providers_.random,
                         std::move(config));
  }

  DatacenterSurvey::Providers providers_;
  PingProfile profile_;
  std::shared_ptr<const SecretString> lock_key_;
  std::unique_ptr<Resolution> resolution_;
  std::unique_ptr<PingJob> probe_;
  bool failed_{};
};

auto PingProfile::envelope() const noexcept
    -> net::protocol::takion::PacketEnvelope {
  if (service_ == PingService::legacy_cloud_stream)
    return net::protocol::takion::PsnEnvelope{1};
  return net::protocol::takion::PlainEnvelope{};
}

auto PingProfile::senkusha_config() const
    -> net::protocol::senkusha::ClientConfig {
  net::protocol::senkusha::ClientConfig config;
  config.profile.bootstrap_version =
      net::protocol::takion::senkusha_client_takion_version;
  config.profile.profile_selection = net::protocol::takion::FixedTakionProfile{
      net::protocol::takion::senkusha_client_takion_version};
  config.profile.envelope = envelope();
  config.profile.receive_envelope = net::protocol::takion::PlainEnvelope{};
  config.profile.big_response =
      net::protocol::senkusha::BigResponsePolicy::fragmented_big_echo;
  config.profile.big.client_version =
      net::protocol::takion::senkusha_client_takion_version;
  config.profile.handoff =
      net::protocol::senkusha::TransportHandoffPolicy::close_after_use;
  config.profile.rtt_adjustment.subtract = psn::gk::Ping::reported_rtt_offset;
  config.profile.rtt_adjustment.floor = psn::gk::Ping::reported_rtt_floor;
  config.policy.probe.count = net::protocol::senkusha::default_probe_count;
  config.policy.probe.tag_offset = probe_tag_offset;
  config.policy.probe.require_unit_index = false;
  config.policy.mtu.lower_bound = net::protocol::senkusha::mtu_lower_bound;
  config.policy.mtu.upper_bound = net::protocol::senkusha::mtu_upper_bound;
  config.policy.mtu.attempts_per_candidate =
      net::protocol::senkusha::mtu_attempts_per_candidate;
  config.policy.mtu.timeout_floor = mtu_timeout_floor;
  config.policy.mtu.timeout_ceiling = mtu_timeout_ceiling;
  config.policy.bandwidth = bandwidth_;
  return config;
}

auto PingTarget::resolve(platform::DnsProvider& dns,
                         const psn::gk::Datacenter& datacenter)
    -> Expected<PingTarget, Error> {
  return lookup(dns, datacenter, nullptr);
}

auto PingTarget::resolve(platform::DnsProvider& dns,
                         const psn::gk::Datacenter& datacenter,
                         const platform::StopToken& stop)
    -> Expected<PingTarget, Error> {
  return lookup(dns, datacenter, &stop);
}

auto PingTarget::lookup(platform::DnsProvider& dns,
                        const psn::gk::Datacenter& datacenter,
                        const platform::StopToken* stop)
    -> Expected<PingTarget, Error> {
  if (datacenter.public_ip.empty()) return unexpected(Error::invalid_host);
  if (datacenter.port > max_port) return unexpected(Error::invalid_port);
  const auto port = datacenter.port > 0 ? datacenter.port : int{senkusha_port};
  const auto service = std::to_string(port);
  const auto answers = stop ? dns.resolve(datacenter.public_ip, service, *stop)
                            : dns.resolve(datacenter.public_ip, service);
  if (!answers) return unexpected(Error::dns_failure);
  const auto ipv4 = std::ranges::find_if(
      answers.value(), [](const auto& address) { return address.is_ipv4(); });
  if (ipv4 == answers.value().end()) return unexpected(Error::no_ipv4_address);
  return PingTarget{
      platform::SocketEndpoint{*ipv4, static_cast<std::uint16_t>(port)}};
}

auto PingJob::over(
    std::unique_ptr<net::transport::datagram::DatagramTransport> transport,
    platform::ClockProvider& clock, platform::ThreadingProvider& threading,
    platform::SecureRandomProvider& random,
    net::protocol::senkusha::ClientConfig config) -> std::unique_ptr<PingJob> {
  return std::make_unique<SenkushaJob>(std::move(transport), clock, threading,
                                       random, std::move(config));
}

auto DatacenterSurvey::start(
    Providers providers, std::vector<psn::gk::Datacenter> datacenters,
    PingService service, const SecretString& lock_key,
    const platform::StopToken& stop,
    std::optional<net::protocol::senkusha::BandwidthPolicy> bandwidth)
    -> Expected<DatacenterSurvey, Error> {
  if (stop.stop_requested()) return unexpected(Error::cancelled);
  if (lock_key.empty()) return unexpected(Error::invalid_key);
  const auto shared_key =
      std::make_shared<const SecretString>(lock_key.clone());

  Jobs jobs;
  jobs.reserve(datacenters.size());
  for (const auto& datacenter : datacenters) {
    if (datacenter.public_ip.empty() || datacenter.port > max_port) {
      jobs.push_back(std::make_unique<FailedJob>());
      continue;
    }
    auto resolution = Resolution::start(providers.threading,
                                        providers.network.dns(), datacenter);
    if (!resolution) {
      jobs.push_back(std::make_unique<FailedJob>());
      continue;
    }
    jobs.push_back(std::make_unique<ResolvingJob>(
        providers, std::move(resolution), PingProfile{service, bandwidth},
        shared_key));
  }
  return start(providers.clock, std::move(datacenters), std::move(jobs));
}

auto DatacenterSurvey::start(const platform::ClockProvider& clock,
                             std::vector<psn::gk::Datacenter> datacenters,
                             Jobs jobs) -> Expected<DatacenterSurvey, Error> {
  const auto fail = [&jobs](Error error) -> Expected<DatacenterSurvey, Error> {
    for (auto& job : jobs)
      if (job) job->cancel();
    return unexpected(error);
  };
  if (jobs.size() != datacenters.size()) return fail(Error::count_mismatch);
  if (std::ranges::any_of(jobs, [](const auto& job) { return !job; }))
    return fail(Error::null_job);
  const auto start_us = clock.now_microseconds();
  if (start_us > std::numeric_limits<std::uint64_t>::max() - window_us)
    return fail(Error::deadline_overflow);
  return DatacenterSurvey(clock, std::move(datacenters), std::move(jobs),
                          start_us + window_us);
}

DatacenterSurvey::DatacenterSurvey(const platform::ClockProvider& clock,
                                   std::vector<psn::gk::Datacenter> datacenters,
                                   Jobs jobs, std::uint64_t deadline_us)
    : clock_(&clock),
      datacenters_(std::move(datacenters)),
      jobs_(std::move(jobs)),
      active_(jobs_.size(), true),
      rows_(jobs_.size()),
      deadline_us_(deadline_us) {}

DatacenterSurvey::DatacenterSurvey(DatacenterSurvey&& other) noexcept
    : clock_(other.clock_),
      datacenters_(std::move(other.datacenters_)),
      jobs_(std::move(other.jobs_)),
      active_(std::move(other.active_)),
      rows_(std::move(other.rows_)),
      deadline_us_(other.deadline_us_),
      finished_(std::exchange(other.finished_, true)) {}

DatacenterSurvey::~DatacenterSurvey() { cancel_active(); }

auto DatacenterSurvey::reported() const noexcept -> std::size_t {
  return static_cast<std::size_t>(std::ranges::count_if(
      rows_, [](const auto& row) { return row.has_value(); }));
}

auto DatacenterSurvey::run(net::http::PollWaiter& waiter,
                           const platform::StopToken& stop,
                           const Measured& measured)
    -> Expected<Result, Error> {
  constexpr int wait_milliseconds = 1;
  const auto total = jobs_.size();
  std::size_t told = 0;
  for (;;) {
    if (stop.stop_requested()) break;
    auto result = step();
    if (const auto done = reported(); measured && done != told) {
      told = done;
      measured(done, total);
    }
    if (result) return std::move(*result);
    const auto sources = pollables();
    if (!waiter.wait_readable(sources, wait_milliseconds, stop) ||
        stop.stop_requested())
      break;
  }
  static_cast<void>(cancel());
  return unexpected(Error::cancelled);
}

auto DatacenterSurvey::step() -> std::optional<Result> {
  if (finished_) return std::nullopt;
  if (clock_->now_microseconds() >= deadline_us_) {
    cancel_active();
    const bool timed_out = !all_reported();
    return Result{take_rows(), timed_out};
  }
  for (std::size_t index = 0; index < jobs_.size(); ++index) {
    if (!active_[index]) continue;
    const auto finished = jobs_[index]->poll();
    if (!finished) continue;
    active_[index] = false;
    rows_[index] = *finished
                       ? measured_row(datacenters_[index], finished->value())
                       : unmeasured_row(datacenters_[index]);
  }
  if (!all_reported()) return std::nullopt;
  return Result{take_rows(), false};
}

auto DatacenterSurvey::cancel() -> std::optional<std::vector<psn::gk::Ping>> {
  if (finished_) return std::nullopt;
  cancel_active();
  return take_rows();
}

void DatacenterSurvey::cancel_active() noexcept {
  for (std::size_t index = 0; index < jobs_.size(); ++index)
    if (active_[index]) {
      active_[index] = false;
      jobs_[index]->cancel();
    }
}

auto DatacenterSurvey::pollables() const
    -> std::vector<const platform::Pollable*> {
  std::vector<const platform::Pollable*> sources;
  if (finished_) return sources;
  for (std::size_t index = 0; index < jobs_.size(); ++index)
    if (active_[index])
      if (const auto* source = jobs_[index]->pollable())
        sources.push_back(source);
  return sources;
}

auto DatacenterSurvey::all_reported() const noexcept -> bool {
  return std::ranges::all_of(rows_,
                             [](const auto& row) { return row.has_value(); });
}

auto DatacenterSurvey::take_rows() -> std::vector<psn::gk::Ping> {
  std::vector<psn::gk::Ping> rows;
  rows.reserve(rows_.size());
  for (std::size_t index = 0; index < rows_.size(); ++index)
    rows.push_back(rows_[index] ? std::move(*rows_[index])
                                : unmeasured_row(datacenters_[index]));
  finished_ = true;
  return rows;
}

}  // namespace libchiaking::play
