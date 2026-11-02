#include "src/core/play/adapters/links/link_driver.hpp"

#include "src/core/media/video_intake.hpp"

#include <libchiaking/platform/runtime/threading.hpp>
#include "libchiaking/detail/secret_string.hpp"
#include "src/core/net/protocol/remote_play/stream/microphone_sender.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::play {

static_assert(LinkDriver::microphone_frame_size ==
              net::protocol::remote_play::MicrophoneSender::frame_size);
static_assert(LinkDriver::max_pending_inputs > 0);

using platform::LogSeverity;

class LinkDriver::Dispatch {
 public:
  explicit Dispatch(std::atomic<int>& depth) noexcept : depth_(depth) {
    depth_.fetch_add(1, std::memory_order_acq_rel);
  }

  ~Dispatch() { depth_.fetch_sub(1, std::memory_order_acq_rel); }

  Dispatch(const Dispatch&) = delete;
  auto operator=(const Dispatch&) -> Dispatch& = delete;

 private:
  std::atomic<int>& depth_;
};

auto LinkDriver::require(std::unique_ptr<Link> session)
    -> std::unique_ptr<Link> {
  if (!session) throw std::invalid_argument{"LinkDriver requires a session"};
  return session;
}

class LinkDriver::Turn {
 public:
  explicit Turn(LinkDriver& driver) : driver_(driver) {
    driver_.session_mutex_.lock();
    driver_.session_owner_.store(platform::ThreadIdentity::current(),
                                 std::memory_order_release);
  }

  struct Taken {};

  Turn(LinkDriver& driver, Taken) noexcept : driver_(driver) {
    driver_.session_owner_.store(platform::ThreadIdentity::current(),
                                 std::memory_order_release);
  }

  ~Turn() {
    driver_.publish();
    driver_.session_owner_.store(platform::ThreadIdentity{},
                                 std::memory_order_release);
    driver_.session_mutex_.unlock();
  }

  Turn(const Turn&) = delete;
  auto operator=(const Turn&) -> Turn& = delete;

 private:
  LinkDriver& driver_;
};

LinkDriver::LinkDriver(std::unique_ptr<Link> session, platform::StopSource stop,
                       platform::ThreadingProvider& threading,
                       LinkDriverSink& sink, platform::DiagnosticsProvider& log,
                       std::optional<Venue> venue)
    : stop_(std::move(stop)),
      sink_(sink),
      log_(log),
      start_message_(venue ? venue->start_message()
                           : std::string_view{"Starting stream connection"}),
      session_mutex_(threading),
      inputs_(max_pending_inputs),
      session_(require(std::move(session))) {
  session_->set_event_sink(*this);
}

LinkDriver::~LinkDriver() { stop_.request_stop(); }

void LinkDriver::on_event(net::protocol::remote_play::StreamEvent event) {
  const Dispatch dispatch{dispatch_depth_};
  if (const auto* changed =
          std::get_if<net::protocol::remote_play::event::StateChanged>(&event))
    published_state_.store(changed->state, std::memory_order_release);
  if (auto* media =
          std::get_if<net::protocol::remote_play::event::EncodedMedia>(
              &event)) {
    if (auto frame = media::VideoIntake::frame(std::move(*media))) {
      sink_.on_video_frame(std::move(*frame));
      return;
    }
  }
  sink_.on_event(std::move(event));
}

auto LinkDriver::owns_session() const noexcept -> bool {
  return session_owner_.load(std::memory_order_acquire) ==
         platform::ThreadIdentity::current();
}

void LinkDriver::publish() noexcept {
  published_state_.store(session_->state(), std::memory_order_release);
}

void LinkDriver::drain_inputs() {
  QueuedInput input;
  std::size_t rejected = 0;
  for (std::size_t ran = 0; ran < max_pending_inputs && inputs_.try_pop(input);
       ++ran) {
    if (!run(*input)) {
      ++rejected;
    }
    input.reset();
  }
  if (rejected != 0) {
    log_.write({LogSeverity::verbose,
                "queued host input was rejected by the session"});
  }
}

void LinkDriver::discard_inputs() noexcept {
  QueuedInput input;
  while (inputs_.try_pop(input)) {
    input.reset();
  }
}

auto LinkDriver::run(Input& input) -> Result {
  LinkInputs& session = *session_;
  return std::visit(
      [&session](auto& queued) -> Result {
        using Queued = std::decay_t<decltype(queued)>;
        if constexpr (std::is_same_v<Queued, ControllerInput>) {
          return session.send_controller_state(queued.state);
        } else if constexpr (std::is_same_v<Queued, PinInput>) {
          return session.submit_pin(queued.pin.bytes());
        } else if constexpr (std::is_same_v<Queued, VideoLossInput>) {
          return session.report_video_loss(queued.first, queued.last);
        } else if constexpr (std::is_same_v<Queued, CreateInput>) {
          return session.send_create(queued.press);
        } else if constexpr (std::is_same_v<Queued, DeepLinkInput>) {
          return session.send_deep_link(std::move(queued.text));
        } else if constexpr (std::is_same_v<Queued, InvitationInput>) {
          return session.send_invitation(std::move(queued.invitation_id),
                                         std::move(queued.session_id));
        } else if constexpr (std::is_same_v<Queued, MicrophoneInput>) {
          return session.send_microphone(queued.frame);
        } else if constexpr (std::is_same_v<Queued, ConnectMicrophoneInput>) {
          return session.connect_microphone();
        } else if constexpr (std::is_same_v<Queued, SystemMenuInput>) {
          return session.toggle_system_menu(queued.pad);
        } else if constexpr (std::is_same_v<Queued, SavedDataInput>) {
          return session.open_saved_data();
        } else if constexpr (std::is_same_v<Queued, MuteInput>) {
          return session.set_microphone_muted(queued.muted);
        } else if constexpr (std::is_same_v<Queued, KeyframeInput>) {
          return session.request_idr();
        } else if constexpr (std::is_same_v<Queued, DropFrameInput>) {
          return session.drop_video_frame();
        } else if constexpr (std::is_same_v<Queued, AudioStateInput>) {
          return session.send_audio_state(queued.kind, queued.body);
        } else if constexpr (std::is_same_v<Queued, KeyboardInput>) {
          return session.send_keyboard(queued.state);
        } else {
          static_assert(std::is_same_v<Queued, CommandInput>);
          return session.execute(queued.command);
        }
      },
      input);
}

auto LinkDriver::submit(Input input) -> Result {
  if (!owns_session() && session_mutex_.try_lock()) {
    const Turn turn{*this, Turn::Taken{}};
    drain_inputs();
    if (inputs_.empty()) {
      return run(input);
    }
  }
  if (!inputs_.try_push(QueuedInput{std::move(input)})) {
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  }
  session_->wake();
  return {};
}

auto LinkDriver::run_stop(bool sleep)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  log_.write({LogSeverity::info,
              sleep ? "session sleep stop begin" : "session stop begin"});
  auto result = sleep ? session_->stop_for_sleep() : session_->stop();
  deferred_stop_.store(DeferredStop::none, std::memory_order_release);
  discard_inputs();
  log_.write(
      {result ? LogSeverity::info : LogSeverity::error,
       sleep ? (result ? "session sleep stop succeeded"
                       : "session sleep stop failed")
             : (result ? "session stop succeeded" : "session stop failed")});
  return result;
}

auto LinkDriver::defer_nested_stop(bool sleep) -> bool {
  if (dispatch_depth_.load(std::memory_order_acquire) <= 0) return false;
  stop_.request_stop();
  const auto wanted = sleep ? DeferredStop::sleep : DeferredStop::stop;
  auto held = deferred_stop_.load(std::memory_order_acquire);
  while (held < wanted && !deferred_stop_.compare_exchange_weak(
                              held, wanted, std::memory_order_acq_rel)) {
  }
  log_.write({LogSeverity::info,
              "session stop requested from a callback; deferred until the "
              "callback returns"});
  session_->wake();
  return true;
}

auto LinkDriver::finish_deferred_stop()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  const auto deferred = deferred_stop_.load(std::memory_order_acquire);
  if (deferred == DeferredStop::none) return {};
  return run_stop(deferred == DeferredStop::sleep);
}

auto LinkDriver::start()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (owns_session())
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  log_.write({LogSeverity::info, start_message_});
  log_.write({LogSeverity::info, "session start begin"});
  const Turn turn{*this};
  auto result = session_->start();
  log_.write({result ? LogSeverity::info : LogSeverity::error,
              result ? "session start succeeded" : "session start failed"});
  const auto stopped = finish_deferred_stop();
  if (!result) return result;
  if (!stopped) return stopped;
  const auto state = session_->state();
  if (state == net::protocol::remote_play::StreamState::stopping ||
      state == net::protocol::remote_play::StreamState::stopped)
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  return {};
}

auto LinkDriver::pump_once()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (owns_session())
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  const Turn turn{*this};
  drain_inputs();
  auto result = session_->pump_once();
  const auto stopped = finish_deferred_stop();
  if (!result) return result;
  return stopped;
}

auto LinkDriver::wait(std::chrono::milliseconds timeout)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (stop_.stop_requested()) return {};
  if (owns_session())
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  const Turn turn{*this};
  if (!inputs_.empty()) {
    return {};
  }
  return session_->wait(timeout);
}

auto LinkDriver::request_stop(bool sleep)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (defer_nested_stop(sleep)) return {};
  stop_.request_stop();
  if (owns_session())
    return unexpected(net::protocol::remote_play::StreamError::invalid_state);
  const Turn turn{*this};
  return run_stop(sleep);
}

auto LinkDriver::stop()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return request_stop(false);
}

auto LinkDriver::stop_for_sleep()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return request_stop(true);
}

void LinkDriver::cancel() noexcept {
  log_.write({LogSeverity::info, "session cancellation requested"});
  stop_.request_stop();
  session_->wake();
}

void LinkDriver::wake() noexcept { session_->wake(); }

auto LinkDriver::select_datacenter(std::string_view name)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  if (name.empty() || name.size() > DatacenterName::limit ||
      name.find('\0') != std::string_view::npos)
    return unexpected(
        net::protocol::remote_play::StreamError::invalid_argument);
  auto result = session_->select_datacenter(name);
  if (result)
    log_.write({LogSeverity::info,
                "datacenter choice accepted: " + std::string{name}});
  return result;
}

auto LinkDriver::state() const noexcept
    -> net::protocol::remote_play::StreamState {
  return published_state_.load(std::memory_order_acquire);
}

auto LinkDriver::diagnostics()
    -> net::protocol::remote_play::StreamDiagnostics {
  if (owns_session()) return session_->diagnostics();
  const Turn turn{*this};
  return session_->diagnostics();
}

auto LinkDriver::failure() const noexcept -> const std::optional<LinkFailure>& {
  return session_->failure();
}

auto LinkDriver::cleanup_failure() const noexcept
    -> const std::optional<LinkFailure>& {
  return session_->cleanup_failure();
}

auto LinkDriver::quit() const noexcept
    -> const net::protocol::remote_play::QuitDetail& {
  return session_->quit();
}

auto LinkDriver::takion_version() const noexcept
    -> std::optional<std::uint32_t> {
  return session_->takion_version();
}

auto LinkDriver::ecdh_curve() const noexcept
    -> std::optional<platform::EcdhCurve> {
  return session_->ecdh_curve();
}

auto LinkDriver::send_controller_state(
    const net::protocol::remote_play::ControllerState& state)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(ControllerInput{state});
}

auto LinkDriver::submit_pin(std::span<const std::byte> pin)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  PinInput input;
  if (!pin.empty()) {
    input.pin.assign(pin);
  }
  return submit(std::move(input));
}

auto LinkDriver::report_video_loss(std::uint16_t first, std::uint16_t last)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(VideoLossInput{first, last});
}

auto LinkDriver::send_create(CloudMessaging::CapturePress press)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(CreateInput{press});
}

auto LinkDriver::send_deep_link(std::string text)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(DeepLinkInput{std::move(text)});
}

auto LinkDriver::send_invitation(std::string invitation_id,
                                 std::string session_id)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(
      InvitationInput{std::move(invitation_id), std::move(session_id)});
}

auto LinkDriver::send_microphone(
    std::span<const std::byte, microphone_frame_size> opus_frame)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  MicrophoneInput input;
  std::ranges::copy(opus_frame, input.frame.begin());
  return submit(input);
}

auto LinkDriver::open_saved_data()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(SavedDataInput{});
}

auto LinkDriver::toggle_system_menu(std::uint32_t pad)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(SystemMenuInput{pad});
}

auto LinkDriver::connect_microphone()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(ConnectMicrophoneInput{});
}

auto LinkDriver::set_microphone_muted(bool muted)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(MuteInput{muted});
}

auto LinkDriver::drop_video_frame()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(DropFrameInput{});
}

auto LinkDriver::request_idr()
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(KeyframeInput{});
}

auto LinkDriver::send_audio_state(
    net::protocol::remote_play::AudioStateKind kind,
    std::span<const std::byte> body)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(
      AudioStateInput{kind, std::vector<std::byte>(body.begin(), body.end())});
}

auto LinkDriver::send_keyboard(const KeyboardState& state)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(KeyboardInput{state});
}

auto LinkDriver::execute(net::protocol::remote_play::StreamCommand command)
    -> Expected<void, net::protocol::remote_play::StreamError> {
  return submit(CommandInput{std::move(command)});
}

}  // namespace libchiaking::play
