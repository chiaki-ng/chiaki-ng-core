#include "src/core/kakusan/guest_visit_visiting.hpp"

namespace libchiaking::kakusan {

class GuestVisit::Visiting::SignalPause final : public Pause {
 public:
  SignalPause(platform::ThreadingProvider& threading,
              const platform::ClockProvider& clock)
      : mutex_(threading), signal_(threading, clock) {}

  void wait(std::chrono::milliseconds limit) override {
    platform::Lock lock{mutex_};
    static_cast<void>(signal_.wait_for(lock, limit, [this] { return woken_; }));
    woken_ = false;
  }

  void wake() noexcept override {
    {
      const platform::Lock lock{mutex_};
      woken_ = true;
    }
    signal_.notify_all();
  }

 private:
  platform::Mutex mutex_;
  platform::ConditionVariable signal_;
  bool woken_{};
};

GuestVisit::Visiting::Visiting(std::shared_ptr<Surroundings> surroundings,
                               std::unique_ptr<Connections> connections,
                               std::unique_ptr<Core> machine,
                               InvitationSummary host, Ports ports,
                               Pacing pacing)
    : surroundings_(std::move(surroundings)),
      connections_(std::move(connections)),
      host_(std::move(host)),
      own_pause_(ports.pause != nullptr
                     ? nullptr
                     : std::make_unique<SignalPause>(
                           surroundings_->threading(), surroundings_->steady())),
      pause_(ports.pause != nullptr ? *ports.pause : *own_pause_),
      gate_(surroundings_->threading()),
      mutex_(surroundings_->threading()),
      machine_(std::move(machine)),
      stream_mutex_(surroundings_->threading()),
      join_(*this, *surroundings_, ports.observer, ports.media, pause_,
            pacing) {}

GuestVisit::Visiting::~Visiting() {
  const platform::Lock turn{gate_.mutex()};
  const platform::Lock lock{mutex_};
  queued_.clear();
}

auto GuestVisit::Visiting::reply(SecretString block) -> Reply {
  return Reply{std::move(block)};
}

auto GuestVisit::Visiting::outcome(VisitOutcome::Status status,
                                   std::optional<Refusal> refusal,
                                   std::optional<media::MediaFault> media)
    -> VisitOutcome {
  return VisitOutcome{status, refusal, std::move(media)};
}

void GuestVisit::Visiting::host_seen() const {
  if (host_key_)
    surroundings_->remembered().seen(*host_key_, surroundings_->wall());
}

void GuestVisit::Visiting::leave_by(std::uint64_t deadline_us) noexcept {
  phase_ = Phase::leaving;
  leave_deadline_us_ = deadline_us;
}

auto GuestVisit::Visiting::note(const VisitChanged& changed) -> Noted {
  progress_ = changed.progress;
  Noted noted;
  noted.connected =
      changed.progress == Progress::playing && !std::exchange(connected_, true);
  noted.finished = changed.progress == Progress::gone &&
                   phase_ != Phase::finished && phase_ != Phase::closed;
  if (noted.finished) {
    refusal_ = changed.refusal;
    phase_ = Phase::finished;
  }
  return noted;
}

auto GuestVisit::Visiting::abandon() -> std::unique_ptr<Core> {
  phase_ = Phase::closed;
  queued_.clear();
  return std::move(machine_);
}

auto GuestVisit::Visiting::close() -> Closed {
  Closed closed;
  closed.refusal = refusal_;
  phase_ = Phase::closed;
  queued_.clear();
  const platform::Lock stream{stream_mutex_};
  if (stream_) {
    if (!fault_) fault_ = stream_->fault();
    statistics_ = stream_->statistics();
  }
  stream_.reset();
  closed.machine = std::move(machine_);
  return closed;
}

auto GuestVisit::Visiting::take_queued() -> std::deque<Step> {
  std::deque<Step> ready;
  ready.swap(queued_);
  return ready;
}

void GuestVisit::Visiting::follow_stream() {
  const platform::Lock lock{stream_mutex_};
  if (stream_ == machine_->forwarded()) return;
  if (stream_) {
    if (!fault_) fault_ = stream_->fault();
    statistics_ = stream_->statistics();
  }
  stream_ = machine_->forwarded();
}

auto GuestVisit::Visiting::forwarded() const
    -> std::shared_ptr<play::ForwardedStream> {
  const platform::Lock lock{stream_mutex_};
  return stream_;
}

auto GuestVisit::Visiting::fault() const -> std::optional<media::MediaFault> {
  const platform::Lock lock{stream_mutex_};
  if (fault_ || !stream_) return fault_;
  return stream_->fault();
}

auto GuestVisit::Visiting::statistics() const -> media::VideoStatistics {
  const platform::Lock lock{stream_mutex_};
  return stream_ ? stream_->statistics() : statistics_;
}

GuestVisit::GuestVisit(std::unique_ptr<Visiting> visiting) noexcept
    : visiting_(std::move(visiting)) {}

GuestVisit::~GuestVisit() = default;

auto GuestVisit::host() const noexcept -> const InvitationSummary& {
  return visiting_->host();
}

auto GuestVisit::host_key() const -> std::optional<IdentityKey> {
  const platform::Lock lock{visiting_->mutex()};
  return visiting_->host_key();
}

auto GuestVisit::verify(const VerificationCode& theirs)
    -> Expected<void, Refusal> {
  return visiting_->join().verify(theirs);
}

auto GuestVisit::remember_host(std::string_view nickname, Replace replace)
    -> Expected<RememberedDevice, RememberRefusal> {
  std::optional<IdentityKey> key;
  {
    const platform::Lock lock{visiting_->mutex()};
    if (visiting_->host_admitted()) key = visiting_->host_key();
  }
  if (!key)
    return unexpected(RememberRefusal{RememberRefusal::Kind::not_verified, {}});
  auto& surroundings = visiting_->surroundings();
  return surroundings.remembered().remember(*key, nickname, replace,
                                            surroundings.wall());
}

auto GuestVisit::send(const controllers::ControllerState& state)
    -> Expected<void, Refusal> {
  return visiting_->join().send(state);
}

auto GuestVisit::run() -> VisitOutcome { return visiting_->join().run(); }

auto GuestVisit::leave() -> Expected<void, Refusal> {
  return visiting_->join().leave();
}

void GuestVisit::cancel() noexcept { visiting_->join().cancel(); }

auto GuestVisit::video_presented() noexcept
    -> std::optional<media::PresentationLoss> {
  const auto stream = visiting_->forwarded();
  if (!stream) {
    return std::nullopt;
  }
  const auto loss = stream->video_presented();
  if (visiting_->cancelled()) {
    return std::nullopt;
  }
  return loss;
}

void GuestVisit::video_rejected(std::uint16_t frame, int cause) {
  if (const auto stream = visiting_->forwarded())
    stream->video_rejected(frame, cause);
  visiting_->pause().wake();
}

void GuestVisit::video_output_failed(int cause) {
  if (const auto stream = visiting_->forwarded())
    stream->video_output_failed(cause);
  visiting_->pause().wake();
}

auto GuestVisit::video_statistics() const -> media::VideoStatistics {
  return visiting_->statistics();
}

}  // namespace libchiaking::kakusan
