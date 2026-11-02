#include "src/core/play/adapters/links/internet/internet_pairer_core.hpp"

#include "src/core/play/adapters/links/internet/internet_link.hpp"
#include "src/core/play/adapters/links/internet/internet_start.hpp"
#include "src/core/play/adapters/links/link_stages.hpp"
#include "src/core/play/adapters/start/link_request.hpp"
#include "src/core/psn/remote_play/signalling_service.hpp"

#include <chrono>
#include <utility>
#include <variant>

namespace libchiaking::play {

class InternetPairer::Core::Attempt::Events final : public LinkDriverSink {
 public:
  Events(std::atomic<Stage>& stage, consoles::PairingWatcher* watcher) noexcept
      : stage_(stage), watcher_(watcher) {}

  void on_event(net::protocol::remote_play::StreamEvent event) override {
    if (const auto* progress =
            std::get_if<net::protocol::remote_play::event::SetupProgress>(
                &event)) {
      reached(LinkStages::pairing_stage(progress->stage));
      told(*progress);
      return;
    }
    if (auto* registered =
            std::get_if<net::protocol::remote_play::event::Registered>(
                &event)) {
      console = std::move(registered->console);
      reached(Stage::pairing);
    }
  }

  void on_video_frame(media::VideoFrame) override {}

  std::optional<net::protocol::remote_play::RegisteredConsole> console;

 private:
  void reached(Stage stage) {
    if (stage_.exchange(stage, std::memory_order_acq_rel) != stage &&
        watcher_ != nullptr)
      watcher_->on_stage(stage);
  }

  void told(const net::protocol::remote_play::event::SetupProgress& progress) {
    const auto step = LinkStages::pairing_step(progress.stage);
    if (watcher_ == nullptr || !step) return;
    consoles::PairingProgress reported{
        .step = *step,
        .elapsed =
            std::chrono::microseconds{
                static_cast<std::chrono::microseconds::rep>(
                    progress.elapsed_us)},
        .route = progress.route};
    if (*step == consoles::PairingStep::waking_console)
      reported.wait = progress::Wait{progress::WaitReason::console_joining};
    watcher_->on_progress(reported);
  }

  std::atomic<Stage>& stage_;
  consoles::PairingWatcher* watcher_;
};

class InternetPairer::Core::Attempt::Stop final : public platform::StopState {
 public:
  Stop(platform::StopToken own, platform::StopToken caller) noexcept
      : own_(std::move(own)), caller_(std::move(caller)) {}

  bool stop_requested() const noexcept override {
    return own_.stop_requested() || caller_.stop_requested();
  }
  bool deadline_expired() const noexcept override {
    return caller_.deadline_expired();
  }

 private:
  platform::StopToken own_;
  platform::StopToken caller_;
};

auto InternetPairer::Core::Attempt::Failure::typed() const
    -> consoles::PairingFailure {
  using Reason = consoles::PairingFailure::Reason;
  consoles::PairingFailure failure{Reason::unreachable};
  failure.internet_stage = stage;
  if (cancelled || (detail && detail->cancelled())) {
    failure.reason = Reason::cancelled;
    return failure;
  }
  if (!detail) {
    if (error ==
            net::protocol::remote_play::StreamError::invalid_configuration ||
        error == net::protocol::remote_play::StreamError::invalid_argument)
      failure.reason = Reason::malformed;
    else if (error ==
             net::protocol::remote_play::StreamError::provider_unavailable)
      failure.reason = Reason::not_available;
    return failure;
  }
  const auto status = detail->http_status();
  if (status != 0) failure.http_status = status;
  if (const auto reason = detail->console_reason()) failure.ps_reason = reason;
  if (detail->stage() ==
      net::protocol::remote_play::FailureStage::registration) {
    using E = play::internet::PsnRemoteRegistrationError;
    const auto* registering =
        detail->as<play::internet::PsnRemoteRegistrationFailure>();
    if (failure.http_status || failure.ps_reason)
      failure.reason = Reason::refused;
    else if (registering && registering->kind == E::timed_out)
      failure.reason = Reason::timed_out;
    else if (registering && registering->kind == E::response_failed)
      failure.reason = Reason::unreadable_reply;
    return failure;
  }
  const auto* psn = detail->as<PsnFailure>();
  if ((psn && psn->lacks_access()) || status == 401 || status == 403) {
    failure.reason = Reason::unanswered;
    failure.unanswered = users::Unanswered{
        users::Unanswered::Reason::remote_play_access_missing, status};
  } else if (status == 429) {
    failure.reason = Reason::unanswered;
    failure.unanswered =
        users::Unanswered{users::Unanswered::Reason::rate_limited, status};
  }
  return failure;
}

InternetPairer::Core::Attempt::Attempt(platform::PlatformFactory& platform,
                                       platform::DiagnosticsProvider& log,
                                       consoles::PairingWatcher* watcher,
                                       platform::StopToken caller)
    : platform_(platform),
      log_(log),
      watcher_(watcher),
      caller_(std::move(caller)),
      mutex_(platform.threading()) {}

InternetPairer::Core::Attempt::~Attempt() = default;

auto InternetPairer::Core::Attempt::run(Request request)
    -> Expected<net::protocol::remote_play::RegisteredConsole, Failure> {
  const auto firmware = consoles::Firmware::from(request.target);
  if (!firmware)
    return unexpected(Failure{
        .error =
            net::protocol::remote_play::StreamError::invalid_configuration});
  ConsolePreferences preferences;
  preferences.feedback = net::protocol::remote_play::FeedbackMode::standard;
  preferences.idr_on_fec_failure = false;
  auto configuration = InternetLinks::configured(
      {consoles::DeviceUid::main_ps4(), *firmware}, std::move(request.user),
      VideoRequest{}, preferences, PortGuessing{},
      InternetLinkConfiguration::default_start_timeout);
  if (!configuration)
    return unexpected(Failure{
        .error =
            net::protocol::remote_play::StreamError::invalid_configuration});
  configuration.value().device_uid = request.device_uid;
  if (stopped()) return unexpected(Failure{.cancelled = true});
  configuration.value().stop =
      platform::StopToken{std::make_shared<Stop>(stop_.token(), caller_)};
  auto session =
      InternetLinks::pairing_only(std::move(configuration.value()), platform_);
  if (!session) return unexpected(Failure{.error = session.error()});
  return run(std::move(session.value()));
}

auto InternetPairer::Core::Attempt::run(std::unique_ptr<Link> session)
    -> Expected<net::protocol::remote_play::RegisteredConsole, Failure> {
  if (session == nullptr)
    return unexpected(Failure{
        .error = net::protocol::remote_play::StreamError::invalid_argument});
  if (started_.exchange(true, std::memory_order_acq_rel) || stopped())
    return unexpected(Failure{.cancelled = true});
  stage_.store(Stage::starting, std::memory_order_release);
  Events events{stage_, watcher_};
  LinkDriver driver{
      std::move(session), stop_, platform_.threading(), events, log_,
      Venue::internet()};
  {
    const platform::Lock lock{mutex_};
    driver_ = &driver;
  }
  if (stopped()) driver.cancel();
  auto status = driver.start();
  while (status && !events.console && !stopped()) {
    const auto state = driver.state();
    if (state != net::protocol::remote_play::StreamState::establishing_route &&
        state != net::protocol::remote_play::StreamState::negotiating_stream)
      break;
    status = driver.pump_once();
    if (status && !events.console) status = driver.wait(wait_step);
  }
  if (caller_.stop_requested()) cancel();
  (void)driver.stop();
  Failure failure{
      .cancelled = stopped(),
      .error = status ? net::protocol::remote_play::StreamError::invalid_state
                      : status.error(),
      .stage = stage(),
      .detail = driver.failure()};
  if (failure.detail)
    failure.stage = LinkStages::pairing_stage(failure.detail->stage());
  {
    const platform::Lock lock{mutex_};
    driver_ = nullptr;
  }
  if (failure.cancelled || !events.console)
    return unexpected(std::move(failure));
  return std::move(*events.console);
}

void InternetPairer::Core::Attempt::cancel() noexcept {
  cancelled_.store(true, std::memory_order_release);
  stop_.request_stop();
  const platform::Lock lock{mutex_};
  if (driver_ != nullptr) driver_->cancel();
}

auto InternetPairer::Core::Attempt::stopped() const noexcept -> bool {
  return cancelled_.load(std::memory_order_acquire) || caller_.stop_requested();
}

InternetPairer::Core::Turn::Turn(std::atomic<bool>& pairing) noexcept
    : pairing_(pairing),
      taken_(!pairing.exchange(true, std::memory_order_acq_rel)) {}

InternetPairer::Core::Turn::~Turn() {
  if (taken_) pairing_.store(false, std::memory_order_release);
}

InternetPairer::Core::Core(platform::PlatformFactory& platform,
                           platform::DiagnosticsProvider* log)
    : platform_(platform),
      log_(log != nullptr ? *log : platform.diagnostics()),
      sessions_mutex_(platform.threading()) {}

void InternetPairer::Core::watch_sessions(std::function<bool()> playing) {
  const platform::Lock lock(sessions_mutex_);
  session_playing_ = std::move(playing);
}

auto InternetPairer::Core::pair(users::PsUser& as,
                                const consoles::DeviceUid& which,
                                consoles::Generation generation,
                                consoles::PairingWatcher* watcher,
                                const platform::StopToken& stop)
    -> Expected<consoles::PairedConsole, consoles::PairingFailure> {
  using Reason = consoles::PairingFailure::Reason;
  if (!as.has_psn_account())
    return unexpected(consoles::PairingFailure{
        .reason = Reason::unanswered,
        .unanswered =
            users::Unanswered{users::Unanswered::Reason::signed_out}});
  const Turn turn{pairing_};
  if (!turn.taken()) return unexpected(consoles::PairingFailure{Reason::busy});
  std::function<bool()> session_playing;
  {
    const platform::Lock lock(sessions_mutex_);
    session_playing = session_playing_;
  }
  if (session_playing && session_playing())
    return unexpected(consoles::PairingFailure{Reason::session_playing});
  const auto target = consoles::Firmware::newest(generation).target();
  Attempt attempt{platform_, log_, watcher, stop};
  auto console = attempt.run(Attempt::Request{
      std::shared_ptr<users::PsUser>{std::shared_ptr<void>{}, &as},
      which.bytes(), target});
  if (!console) return unexpected(console.error().typed());
  auto paired = InternetStart::paired_console(console.value(), target);
  if (!paired)
    return unexpected(consoles::PairingFailure{
        .reason = Reason::malformed, .internet_stage = Stage::pairing});
  return std::move(*paired);
}

InternetPairer::InternetPairer(platform::PlatformFactory& platform,
                               platform::DiagnosticsProvider* log)
    : core_(std::make_unique<Core>(platform, log)) {}

InternetPairer::~InternetPairer() = default;

auto InternetPairer::pair(users::PsUser& as, const consoles::DeviceUid& which,
                          consoles::Generation generation,
                          consoles::PairingWatcher* watcher,
                          const platform::StopToken& stop)
    -> Expected<consoles::PairedConsole, consoles::PairingFailure> {
  return core_->pair(as, which, generation, watcher, stop);
}

}  // namespace libchiaking::play
