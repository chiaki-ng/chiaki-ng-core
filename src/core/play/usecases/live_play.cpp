#include "src/core/play/usecases/live_play.hpp"

#include <utility>

namespace libchiaking::play::usecases {

LivePlay::LivePlay(consoles::KnownConsoles& known,
                   std::shared_ptr<users::PsUser> player,
                   std::shared_ptr<consoles::Console> console,
                   platform::DiagnosticsProvider* log)
    : player_(std::move(player)), console_(std::move(console)), log_(log) {
  if (console_) reports_.emplace(known, console_, player_);
}

void LivePlay::reached_as(const consoles::DeviceUid& device_uid) {
  device_uid_ = device_uid;
}

auto LivePlay::streams_over(platform::PlatformFactory& host,
                            std::shared_ptr<platform::NetworkProvider> network)
    -> platform::PlatformFactory& {
  if (!network) return host;
  home_network_ = consoles::HomeNetwork{network};
  return stream_platform_.emplace(host, std::move(network));
}

void LivePlay::woken_by(std::function<bool()> waker) {
  scripted_wake_ = std::move(waker);
}

auto LivePlay::seat(std::unique_ptr<Session::Core> core,
                    CloudLaunch::Core* launch) -> Session::Core& {
  core_ = core.get();
  launch_ = launch;
  static_cast<void>(
      core_->attach(static_cast<Session::Core::LinkReports&>(*this)));
  if (launch_ != nullptr)
    static_cast<void>(core_->attach(face_.emplace(*launch_)));
  session_.emplace(std::move(core));
  return *core_;
}

void LivePlay::launch_progress(
    const net::protocol::remote_play::event::SetupProgress&) {
  if (launch_ != nullptr)
    core_->events().on_launch_progress(launch_->progress());
}

void LivePlay::datacenters_measured(
    const net::protocol::remote_play::event::CloudDatacenters&) {
  if (launch_ == nullptr) return;
  if (const auto offer = launch_->offer())
    core_->events().on_datacenters_offered(*offer);
}

void LivePlay::datacenter_selected(
    const net::protocol::remote_play::event::CloudDatacenterSelected&) {
  if (launch_ == nullptr) return;
  if (const auto chosen = launch_->chosen())
    core_->events().on_datacenter_chosen(*chosen);
}

auto LivePlay::launch_refused() -> std::optional<LaunchRefused> {
  return launch_ != nullptr ? launch_->refusal() : std::nullopt;
}

void LivePlay::console_named(std::string_view name) {
  if (reports_) reports_->named(name);
}

auto LivePlay::on_paired(consoles::PairedConsole paired)
    -> Expected<void, Refusal> {
  if (!reports_) return {};
  if (!reports_->recognises(paired))
    return unexpected(Refusal::unpaired_console);
  if (!device_uid_) return {};
  if (!reports_->paired(std::move(paired), *device_uid_) && log_ != nullptr)
    log_->write({platform::LogSeverity::warning,
                 "play: the pairing made by an internet start was not saved"});
  return {};
}

auto LivePlay::wake_console() -> bool {
  if (scripted_wake_) return scripted_wake_();
  return console_ && console_->wake(*player_, home_network_).has_value();
}

auto LivePlay::leaving() const noexcept -> bool {
  return core_ != nullptr && core_->state() == SessionState::leaving;
}

void LivePlay::trying_again(std::uint32_t attempt, bool woke, bool reconnecting,
                            const StartPolicy::Failure& failure,
                            std::chrono::milliseconds rest) {
  core_->starting_again(
      attempt, woke, reconnecting,
      AttemptFailure{failure.cause, failure.quit,
                     StartPolicy::waiting_for_network(failure)},
      rest);
}

}  // namespace libchiaking::play::usecases
