#include "src/core/psn/remote_play/signalling_service.hpp"

#include "src/core/psn/remote_play/holepunch_message.hpp"
#include "src/core/psn/remote_play/push_client.hpp"

#include <algorithm>
#include <boost/sml.hpp>
#include <deque>
#include <limits>
#include <optional>
#include <utility>

namespace libchiaking::psn::remote_play {

auto PushOpenError::kind() const noexcept -> Kind {
  return static_cast<Kind>(cause_.index());
}

PushConnection::PushConnection(
    PushClient& client,
    std::unique_ptr<platform::WebSocketConnection> socket) noexcept
    : client_(&client), socket_(std::move(socket)) {}

auto PushConnection::open(SessionClient& session_client,
                          PushClient& push_client,
                          const psn::auth::BearerToken& bearer_token,
                          const platform::StopToken& stop)
    -> Expected<PushConnection, PushOpenError> {
  if (bearer_token.empty())
    return unexpected(PushOpenError{PushOpenError::InvalidToken{}});
  if (stop.stop_requested())
    return unexpected(PushOpenError{PushOpenError::Cancelled{}});
  auto fqdn = session_client.get_push_fqdn(bearer_token, stop);
  if (!fqdn)
    return unexpected(
        PushOpenError{PushOpenError::GetFqdnFailed{fqdn.error()}});
  auto connection = push_client.connect(bearer_token, fqdn.value(), stop);
  if (!connection)
    return unexpected(
        PushOpenError{PushOpenError::ConnectFailed{connection.error()}});
  return PushConnection{push_client, std::move(connection.value())};
}

auto PushConnection::close() -> Expected<void, platform::WebSocketError> {
  if (!socket_) return {};
  auto closed = socket_->close();
  socket_.reset();
  return closed;
}

SessionLease::SessionLease(SessionLease&& other) noexcept
    : client_(other.client_),
      token_(std::move(other.token_)),
      created_(std::move(other.created_)),
      release_attempted_(std::exchange(other.release_attempted_, true)) {}

auto SessionLease::create(SessionClient& client,
                          psn::auth::BearerToken&& bearer_token,
                          const platform::StopToken& stop)
    -> Expected<SessionLease, SessionError> {
  auto created = client.create_session(bearer_token, stop);
  if (!created) return unexpected(created.error());
  return SessionLease(client, std::move(bearer_token),
                      std::move(created.value()));
}

auto SessionLease::release(const platform::StopToken& stop)
    -> Expected<void, SessionError> {
  if (release_attempted_)
    return unexpected(SessionError{SessionErrorKind::invalid_request});
  release_attempted_ = true;
  return client_.release_session(token_, created_.session_id, stop);
}

SessionLease::SessionLease(SessionClient& client,
                           psn::auth::BearerToken&& token,
                           psn::rest::CreatedSession&& created) noexcept
    : client_(client), token_(std::move(token)), created_(std::move(created)) {}

auto StartError::kind() const noexcept -> Kind {
  return static_cast<Kind>(cause_.index());
}

class StartedSession::Startup {
 public:
  Startup(const platform::ClockProvider& clock, SessionLease&& lease,
          PushConnection&& connection) noexcept
      : clock_(clock),
        lease_(std::move(lease)),
        connection_(std::move(connection)) {}
  Startup(const Startup&) = delete;
  Startup& operator=(const Startup&) = delete;
  ~Startup() {
    if (!settled_) (void)abandon({CreationWaitError::failed, std::nullopt});
  }

  SessionLease& lease() noexcept { return lease_; }
  PushConnection& connection() noexcept { return connection_; }

  auto abandon(const NotificationWaitFailure& failure)
      -> StartError::WaitFailed {
    settled_ = true;
    StartError::WaitFailed result;
    result.wait = failure.kind;
    result.push = failure.push;
    auto& push_client = connection_.client();
    if (!still_open(failure.push) || !connection_ ||
        lease_.release_attempted()) {
      result.release_skipped = true;
    } else {
      auto released = lease_.release({});
      if (!released) result.release_failure = released.error();
      push_client.scope_session({});
      const auto now = clock_.now_microseconds();
      const auto deadline = now > std::numeric_limits<std::uint64_t>::max() -
                                        net::psn_session_deletion_timeout_us
                                ? std::numeric_limits<std::uint64_t>::max()
                                : now + net::psn_session_deletion_timeout_us;
      for (;;) {
        auto notification =
            push_client.receive_bounded(*connection_, {}, clock_, deadline);
        if (!notification) break;
        const auto type = notification.value().type();
        if (type == psn::rest::NotificationType::members_deleted ||
            type == psn::rest::NotificationType::remote_play_session_deleted) {
          result.deletion_confirmed = true;
          break;
        }
      }
    }
    if (connection_) {
      auto closed = connection_.close();
      if (!closed) result.close_failure = closed.error();
    }
    return result;
  }

  auto complete(std::string online_id, std::uint64_t attempt)
      -> StartedSession {
    settled_ = true;
    return StartedSession{std::move(lease_), std::move(connection_),
                          std::move(online_id), attempt};
  }

 private:
  static bool still_open(const std::optional<PushError>& failure) noexcept {
    return !failure || (failure->kind != PushErrorKind::websocket_failure &&
                        failure->kind != PushErrorKind::session_deleted);
  }

  const platform::ClockProvider& clock_;
  SessionLease lease_;
  PushConnection connection_;
  bool settled_{};
};

StartedSession::StartedSession(SessionLease&& lease,
                               PushConnection&& connection,
                               std::string online_id,
                               std::uint64_t attempt) noexcept
    : lease_(std::move(lease)),
      connection_(std::move(connection)),
      online_id_(std::move(online_id)),
      attempt_(attempt) {}

auto StartedSession::open(SessionClient& session_client,
                          PushClient& push_client, CreationBarrier& barrier,
                          psn::auth::BearerToken&& bearer_token,
                          std::uint64_t attempt,
                          const platform::ClockProvider& clock,
                          platform::StopToken parent_stop,
                          net::RemoteWaitDeadline deadline_microseconds)
    -> Expected<StartedSession, StartError> {
  if (attempt == 0 || barrier.attempt() != attempt)
    return unexpected(StartError{StartError::InvalidAttempt{}});
  if (parent_stop.stop_requested())
    return unexpected(StartError{StartError::Cancelled{}});
  const auto open_deadline = deadline_microseconds.arm(clock);
  if (clock.now_microseconds() >= open_deadline)
    return unexpected(StartError{StartError::TimedOut{}});
  const auto open_state = std::make_shared<net::DeadlineStopState>(
      parent_stop, clock, open_deadline);
  const platform::StopToken open_stop{open_state};
  auto connection = PushConnection::open(session_client, push_client,
                                         bearer_token, open_stop);
  if (!connection) {
    if (open_state->timed_out())
      return unexpected(StartError{StartError::TimedOut{}});
    return unexpected(
        StartError{StartError::PushOpenFailed{connection.error()}});
  }
  auto lease =
      SessionLease::create(session_client, std::move(bearer_token), open_stop);
  if (!lease) {
    StartError::CreateFailed error{lease.error(), std::nullopt};
    auto closed = connection.value().close();
    if (!closed) error.close_failure = closed.error();
    if (open_state->timed_out())
      return unexpected(StartError{StartError::TimedOut{}});
    return unexpected(StartError{std::move(error)});
  }
  const platform::StopToken bounded_stop{
      std::make_shared<net::DeadlineStopState>(parent_stop, clock,
                                               deadline_microseconds.absolute)};
  Startup startup{clock, std::move(lease.value()),
                  std::move(connection.value())};
  push_client.scope_session(startup.lease().created().session_id);
  auto online_id = push_client.await_creation(
      barrier, attempt, *startup.connection(), parent_stop, clock,
      deadline_microseconds, [&] {
        return session_client.observe_session(
            startup.lease().bearer_token(),
            startup.lease().created().session_id, SessionView::detailed,
            bounded_stop);
      });
  if (!online_id)
    return unexpected(StartError{startup.abandon(online_id.error())});
  return startup.complete(std::move(online_id.value()), attempt);
}

struct SignallingService::ControlOffer {};

struct SignallingService::ControlSelected {};

struct SignallingService::ControlEstablished {};

struct SignallingService::DataOffer {};

struct SignallingService::DataSelected {};

struct SignallingService::Active {};

struct SignallingService::BeginData {};

struct SignallingService::BeginActive {};

struct SignallingService::Offer {};

struct SignallingService::Accept {};

struct SignallingService::Decision {
  Disposition disposition{Disposition::deliver};
};

struct SignallingService::SignallingFsm {
  auto operator()() const {
    using namespace boost::sml;
    const auto ack = [](Decision& out) {
      out.disposition = Disposition::acknowledge;
    };
    const auto after_control = [](Decision& out) {
      out.disposition = Disposition::after_control;
    };
    return make_transition_table(
        *state<ControlOffer> + event<Offer> = state<ControlSelected>,
        state<ControlSelected> + event<Offer> / ack,
        state<ControlSelected> + event<Accept> = state<ControlEstablished>,
        state<ControlSelected> + event<BeginData> = state<DataOffer>,
        state<ControlEstablished> + event<Offer> / after_control,
        state<ControlEstablished> + event<BeginData> = state<DataOffer>,
        state<DataOffer> + event<Offer> = state<DataSelected>,
        state<DataSelected> + event<Offer> / ack,
        state<DataSelected> + event<BeginActive> = state<Active>,
        state<Active> + event<Offer> / ack);
  }
};

class SignallingService::StopState final : public platform::StopState {
 public:
  explicit StopState(platform::StopToken token) : parent_(std::move(token)) {}
  bool stop_requested() const noexcept override {
    return stopped_.load() || parent_.stop_requested() ||
           desired_.load() != applied_.load();
  }
  bool ending() const { return stopped_.load() || parent_.stop_requested(); }
  void end() { stopped_.store(true); }
  auto desired() const -> PsnServicePhase { return desired_.load(); }
  void desire(PsnServicePhase phase) { desired_.store(phase); }
  auto applied() const -> PsnServicePhase { return applied_.load(); }
  void apply(PsnServicePhase phase) { applied_.store(phase); }

 private:
  platform::StopToken parent_;
  std::atomic<bool> stopped_{};
  std::atomic<PsnServicePhase> desired_{PsnServicePhase::control};
  std::atomic<PsnServicePhase> applied_{PsnServicePhase::control};
};

SignallingService::SignallingService(
    PushFeed& feed, platform::WebSocketConnection& connection,
    SessionClient& session, platform::ThreadingProvider& threading,
    const platform::ClockProvider& clock, std::uint64_t attempt,
    const psn::auth::BearerToken& token, std::string_view session_id,
    std::int64_t account_id, std::array<std::byte, psn_device_uid_size> uid,
    psn::rest::ConsoleFamily family, platform::StopToken parent)
    : feed_(feed),
      connection_(connection),
      session_(session),
      attempt_(attempt),
      token_(token.clone()),
      session_id_(session_id),
      account_id_(account_id),
      uid_(uid),
      family_(family),
      stop_(std::make_shared<StopState>(std::move(parent))),
      mutex_(threading),
      changed_(threading, clock) {}

SignallingService::~SignallingService() { stop(); }

auto SignallingService::start(platform::ThreadingProvider& threading)
    -> Expected<void, platform::PlatformError> {
  platform::native::ThreadEntry task{&entry, this};
  task.role = platform::ThreadRole::control;
  task.name = "chiaki-psn";
  auto thread = threading.start_thread(task);
  if (!thread) return unexpected(thread.error());
  if (!thread.value())
    return unexpected(platform::PlatformError::thread_failure);
  thread_ = std::move(thread.value());
  return {};
}

void SignallingService::phase(std::uint64_t attempt, PsnServicePhase phase) {
  if (attempt != attempt_) {
    log("PSN signalling: rejected stale phase command");
    return;
  }
  auto current = stop_->desired();
  if (phase <= current) {
    log("PSN signalling: rejected duplicate or backward phase command");
    return;
  }
  stop_->desire(phase);
}

void SignallingService::console_uid(
    std::array<std::byte, psn_device_uid_size> uid) {
  platform::Lock lock{mutex_};
  uid_ = uid;
}

void SignallingService::stop() {
  stop_->end();
  {
    const platform::Lock lock{mutex_};
    changed_.notify_all();
  }
  if (thread_ && thread_->joinable()) {
    connection_.interrupt_receive();
    (void)thread_->join();
  }
  thread_.reset();
}

auto SignallingService::failure() const -> std::optional<PushError> {
  platform::Lock lock{mutex_};
  return failure_;
}

void SignallingService::fail(PushError error) {
  platform::Lock lock{mutex_};
  if (!failure_) failure_ = std::move(error);
  changed_.notify_all();
}

void SignallingService::log(std::string_view message) const {
  feed_.note(message);
}

class SignallingService::ReceiveStop final : public platform::StopState {
 public:
  ReceiveStop(platform::StopToken receiver, std::shared_ptr<SignallingService::StopState> state)
      : receiver_(std::move(receiver)), state_(std::move(state)) {}
  bool stop_requested() const noexcept override {
    return receiver_.stop_requested() || state_->ending();
  }

 private:
  platform::StopToken receiver_;
  std::shared_ptr<SignallingService::StopState> state_;
};

auto SignallingService::receive(
    platform::WebSocketConnection& connection, const platform::StopToken& stop,
    std::span<const psn::rest::NotificationType> accepted)
    -> Expected<psn::rest::Notification, PushError> {
  if (&connection != &connection_)
    return unexpected(PushError{PushErrorKind::connection_mismatch});
  const platform::StopToken until{
      std::make_shared<const ReceiveStop>(stop, stop_)};
  platform::Lock lock{mutex_};
  for (;;) {
    if (stop.stop_requested() || stop_->ending())
      return unexpected(PushError{PushErrorKind::cancelled});
    auto taken = inbox_.scan([&](const psn::rest::Notification& item) {
      const bool wanted =
          accepted.empty() ||
          std::ranges::find(accepted, item.type()) != accepted.end();
      return wanted ? NotificationInbox::Verdict::take
                    : NotificationInbox::Verdict::keep;
    });
    if (taken) return std::move(*taken);
    if (failure_) return unexpected(*failure_);
    static_cast<void>(changed_.wait(lock, until));
  }
}

void SignallingService::entry(void* context) noexcept {
  auto& self = *static_cast<SignallingService*>(context);
  try {
    self.run();
  } catch (...) {
    self.fail({PushErrorKind::websocket_failure,
               platform::WebSocketError::backend_failure});
  }
}

class SignallingService::AckStop final : public platform::StopState {
 public:
  explicit AckStop(std::shared_ptr<SignallingService::StopState> state)
      : state_(std::move(state)) {}
  bool stop_requested() const noexcept override { return state_->ending(); }

 private:
  std::shared_ptr<SignallingService::StopState> state_;
};

class SignallingService::Worker {
 public:
  explicit Worker(SignallingService& service)
      : service_(service), machine_(decision_), stop_(service.stop_) {}

  void run() {
    service_.log("PSN signalling: awaiting CTRL offer");
    while (!service_.stop_->ending()) {
      if (!apply_phase()) break;
      auto received = service_.feed_.next(service_.connection_, stop_);
      if (service_.stop_->ending()) break;
      if (!received) {
        if (received.error().kind == PushErrorKind::cancelled &&
            service_.stop_->desired() != service_.stop_->applied())
          continue;
        service_.fail(received.error());
        break;
      }
      if (!apply_phase() || handle(received.value()) == Step::stop) break;
    }
    service_.changed_.notify_all();
    service_.log("PSN signalling: worker stopped");
  }

 private:
  enum class Step : std::uint8_t { next, stop };

  auto awaiting_offer() const -> bool {
    return machine_.is(boost::sml::state<ControlOffer>) ||
           machine_.is(boost::sml::state<DataOffer>);
  }

  auto apply_phase() -> bool {
    const auto next = service_.stop_->desired();
    if (next == service_.stop_->applied()) return true;
    const bool applied = next == PsnServicePhase::data
                             ? machine_.process_event(BeginData{})
                             : machine_.process_event(BeginActive{});
    if (!applied) {
      service_.fail({PushErrorKind::invalid_notification});
      service_.log("PSN signalling: rejected illegal phase transition");
      return false;
    }
    service_.stop_->apply(next);
    service_.log(next == PsnServicePhase::data
                     ? "PSN signalling: awaiting DATA offer"
                     : "PSN signalling: servicing active session");
    if (next == PsnServicePhase::data && !early_offers_.empty())
      release_early_offers();
    if (next == PsnServicePhase::active) {
      platform::Lock lock{service_.mutex_};
      service_.inbox_.clear();
    }
    return true;
  }

  void release_early_offers() {
    static_cast<void>(machine_.process_event(Offer{}));
    platform::Lock lock{service_.mutex_};
    const auto dropped =
        service_.inbox_.push_front(std::exchange(early_offers_, {}));
    early_offer_bytes_ = 0;
    for (std::size_t count = 0; count < dropped; ++count)
      service_.log(
          "PSN signalling: inbox full; dropped OFFER received "
          "before the CTRL route was established");
    service_.log(
        "PSN signalling: delivered OFFER received before the "
        "CTRL route was established");
    service_.changed_.notify_all();
  }

  auto handle(psn::rest::Notification& notification) -> Step {
    service_.feed_.log(notification.type(), "received_by_service");
    if (!PushClient::notification_in_session(notification,
                                             service_.session_id_)) {
      service_.log("PSN signalling: ignored unrelated session notification");
      return Step::next;
    }
    if (notification.type() ==
            psn::rest::NotificationType::remote_play_session_deleted &&
        PushClient::notification_in_session(notification, service_.session_id_,
                                            true)) {
      service_.fail({PushErrorKind::session_deleted});
      service_.log("PSN signalling: active session deleted by peer");
      return Step::stop;
    }
    if (notification.type() ==
        psn::rest::NotificationType::session_message_created) {
      auto message = HolepunchMessage::decode(notification);
      if (!message && !awaiting_offer()) {
        service_.log("PSN signalling: ignored undecodable session message");
        return Step::next;
      }
      if (message) {
        const auto outcome = on_message(message.value(), notification);
        if (outcome) return *outcome;
      }
    }
    if (service_.stop_->applied() != PsnServicePhase::active)
      deliver(std::move(notification));
    return Step::next;
  }

  auto on_message(const net::nat::holepunch::SessionMessage& message,
                  psn::rest::Notification& notification)
      -> std::optional<Step> {
    using net::nat::holepunch::Action;
    if (message.action == Action::accept)
      static_cast<void>(machine_.process_event(Accept{}));
    if (message.action != Action::offer) return std::nullopt;
    if (machine_.is(boost::sml::state<DataOffer>) && control_offer_req_id_ &&
        message.req_id == *control_offer_req_id_) {
      service_.log(
          "PSN signalling: ignored repeated control OFFER "
          "while awaiting DATA offer");
      return Step::next;
    }
    decision_.disposition = Disposition::deliver;
    if (!machine_.process_event(Offer{})) {
      service_.fail({PushErrorKind::invalid_notification});
      return Step::stop;
    }
    if (!control_offer_req_id_) control_offer_req_id_ = message.req_id;
    if (decision_.disposition == Disposition::after_control &&
        message.req_id == *control_offer_req_id_) {
      service_.log(
          "PSN signalling: ignored repeated control OFFER after ACCEPT");
      return Step::next;
    }
    if (decision_.disposition == Disposition::acknowledge)
      return acknowledge(message, notification);
    return std::nullopt;
  }

  auto acknowledge(const net::nat::holepunch::SessionMessage& message,
                   psn::rest::Notification& notification) -> Step {
    const auto uid = [&] {
      platform::Lock lock{service_.mutex_};
      return service_.uid_;
    }();
    auto ack = service_.session_.send_short_message(
        service_.token_, service_.session_id_,
        net::nat::holepunch::Action::result, message.req_id, 0,
        service_.account_id_, uid, service_.family_,
        platform::StopToken{std::make_shared<AckStop>(service_.stop_)});
    if (service_.stop_->ending()) return Step::stop;
    service_.log(ack ? "PSN signalling: repeated OFFER acknowledged"
                     : "PSN signalling: repeated OFFER acknowledgement failed; "
                       "continuing");
    if (machine_.is(boost::sml::state<ControlSelected>) &&
        message.req_id != *control_offer_req_id_)
      hold_early_offer(std::move(notification));
    return Step::next;
  }

  void hold_early_offer(psn::rest::Notification notification) {
    if (early_offers_.size() >= max_inbox_notifications ||
        notification.size() > max_inbox_bytes - early_offer_bytes_) {
      service_.log(
          "PSN signalling: dropped OFFER that arrived before the "
          "CTRL ACCEPT; the held OFFERs are full");
      return;
    }
    early_offer_bytes_ += notification.size();
    early_offers_.push_back(std::move(notification));
    service_.log(
        "PSN signalling: held OFFER that arrived before the CTRL ACCEPT");
  }

  void deliver(psn::rest::Notification notification) {
    platform::Lock lock{service_.mutex_};
    const auto shed =
        service_.inbox_.push_shedding_oldest(std::move(notification));
    if (!shed) {
      service_.log("PSN signalling: dropped oversized notification");
      return;
    }
    for (std::size_t dropped = 0; dropped < *shed; ++dropped)
      service_.log("PSN signalling: inbox full; dropped oldest notification");
    service_.changed_.notify_all();
  }

  SignallingService& service_;
  Decision decision_;
  boost::sml::sm<SignallingFsm> machine_;
  std::optional<std::uint64_t> control_offer_req_id_;
  std::deque<psn::rest::Notification> early_offers_;
  std::size_t early_offer_bytes_{};
  const platform::StopToken stop_;
};

void SignallingService::run() { Worker{*this}.run(); }

}  // namespace libchiaking::psn::remote_play
