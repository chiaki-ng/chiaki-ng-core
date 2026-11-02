#include "src/core/consoles/adapters/pairing/home_pairing.hpp"

#include "libchiaking/platform/crypto/provider.hpp"
#include "libchiaking/platform/runtime/clock_provider.hpp"
#include "libchiaking/platform/runtime/random_provider.hpp"
#include "src/core/net/protocol/remote_play/pairing/limits.hpp"
#include "src/core/net/protocol/remote_play/pairing/request.hpp"
#include "src/core/net/protocol/remote_play/rp_reason.hpp"

#include <algorithm>
#include <format>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::consoles {

auto HomePairing::Request::of(const users::PsUser& as, Firmware firmware,
                              PairingPin pin)
    -> Expected<Request, PairingFailure> {
  Request request{firmware.target(), std::move(pin)};
  if (firmware.family() == Firmware::Family::ps4_8) {
    if (!as.has_psn_account())
      return unexpected(
          PairingFailure{PairingFailure::Reason::needs_online_id});
    request.online_id_ = as.online_id();
    return request;
  }
  const auto account = as.account_id().bytes();
  std::ranges::copy(account, request.account_.emplace().begin());
  return request;
}

auto HomePairing::Request::with(
    net::protocol::remote_play::RegistrationTarget target,
    std::optional<AccountBytes> account, std::optional<std::string> online_id,
    PairingPin pin, std::uint32_t console_pin) -> Request {
  Request request{target, std::move(pin)};
  request.account_ = std::move(account);
  request.online_id_ = std::move(online_id);
  request.console_pin_ = console_pin;
  return request;
}

HomePairing::HomePairing(platform::NetworkProvider& network,
                         platform::ThreadingProvider& threading,
                         platform::CryptoProvider& crypto,
                         const platform::ClockProvider& clock,
                         platform::SecureRandomProvider& random,
                         platform::DiagnosticsProvider* log)
    : dns_(network.dns()),
      crypto_(crypto),
      clock_(clock),
      random_(random),
      log_(log),
      lifecycle_(context_),
      knock_(network, clock),
      exchange_(network.sockets(), crypto, clock),
      waiter_(threading, clock, network) {}

HomePairing::~HomePairing() = default;

auto HomePairing::elapsed_since(std::uint64_t began_us,
                                std::uint64_t now_us) noexcept
    -> std::chrono::microseconds {
  return std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(
      timing::Deadline::elapsed_us(began_us, now_us))};
}

auto HomePairing::reported(Step step) noexcept -> std::optional<PairingStep> {
  switch (step) {
    case Step::knocking:
    case Step::settling:
    case Step::connecting:
      return PairingStep::reaching_console;
    case Step::sending:
      return PairingStep::sending_request;
    case Step::receiving:
      return PairingStep::waiting_for_answer;
    case Step::idle:
    case Step::closing:
    case Step::paired:
    case Step::cancelled:
    case Step::failed:
      break;
  }
  return std::nullopt;
}

auto HomePairing::run(Request request, std::string_view host,
                      const platform::StopToken& stop, std::uint16_t port,
                      std::chrono::milliseconds within, PairingWatcher* watcher)
    -> Expected<PairedConsole, PairingFailure> {
  if (within.count() <= 0)
    return unexpected(PairingFailure{reason_for(Error::invalid_request)});
  if (stop.stop_requested())
    return unexpected(PairingFailure{PairingFailure::Reason::cancelled});
  const auto asked = Firmware::from(request.target())
                         .value_or(Firmware::newest(Generation::ps4));
  const auto began_us = clock_.now_microseconds();
  const auto gives_up = timing::Deadline::after(
      timing::Deadline::instant_of(began_us),
      std::chrono::duration_cast<timing::Deadline::Duration>(within));
  auto told = PairingStep::starting;
  const auto tell = [&](std::optional<PairingStep> reached) {
    if (watcher == nullptr || !reached) return;
    while (told < *reached) {
      told =
          told < PairingStep::reaching_console
              ? PairingStep::reaching_console
              : static_cast<PairingStep>(static_cast<std::uint8_t>(told) + 1);
      PairingProgress progress{
          .step = told,
          .elapsed = elapsed_since(began_us, clock_.now_microseconds())};
      if (told == PairingStep::waiting_for_answer)
        progress.wait = progress::Wait{progress::WaitReason::console_answering};
      watcher->on_progress(progress);
    }
  };
  const auto started = start_at(std::move(request), host, port);
  if (!started) return failed(started.error(), stop);
  tell(reported(step()));
  Expected<void, Error> pumped{};
  for (;;) {
    const auto now = timing::Deadline::now(clock_);
    if (stop.stop_requested())
      (void)cancel();
    else if (gives_up.expired(now))
      give_up();
    pumped = pump_once();
    if (!pumped || terminal(step())) break;
    tell(reported(step()));
    wait(std::min(wait_step, std::chrono::ceil<std::chrono::milliseconds>(
                                 *gives_up.remaining(now))));
  }
  if (stop.stop_requested() || step() == Step::cancelled)
    return unexpected(PairingFailure{PairingFailure::Reason::cancelled});
  if (step() == Step::paired) {
    tell(PairingStep::waiting_for_answer);
    auto console = take_result();
    if (!console) return failed(console.error(), stop);
    return paired(std::move(console.value()), asked);
  }
  if (!pumped) return failed(pumped.error(), stop);
  return failed(last_error_.value_or(Error::invalid_state), stop);
}

auto HomePairing::start(Request request, Door door) -> Expected<void, Error> {
  if (step() != Step::idle) return unexpected(Error::invalid_state);
  target_ = request.target_;
  console_pin_ = request.console_pin_;
  door_ = std::move(door);
  const auto prepared = prepare(request);
  if (!prepared) {
    clear_prepared();
    return unexpected(prepared.error());
  }
  if (!lifecycle_.process(home_pairing_lifecycle::Start{attempt})) {
    clear_prepared();
    return unexpected(Error::invalid_state);
  }
  run_effects();
  return outcome();
}

auto HomePairing::start_at(Request request, std::string_view host,
                           std::uint16_t port, bool broadcast)
    -> Expected<void, Error> {
  const bool needs_account =
      request.target_ ==
          net::protocol::remote_play::RegistrationTarget::ps4_10 ||
      request.target_ == net::protocol::remote_play::RegistrationTarget::ps5_1;
  if (host.empty() || host.size() > host_limit ||
      host.find('\0') != std::string_view::npos ||
      (!request.account_ && (needs_account || !request.online_id_)))
    return unexpected(Error::invalid_request);
  if (step() != Step::idle) return unexpected(Error::invalid_state);
  const bool ipv6 = host.find(':') != std::string_view::npos;
  const auto resolved = dns_.resolve(host, {});
  if (!resolved) return unexpected(Error::network_failure);
  std::vector<platform::SocketEndpoint> endpoints;
  for (const auto& address : resolved.value()) {
    if (address.is_ipv6() == ipv6)
      endpoints.push_back({address, port != 0 ? port : rp_session_port});
  }
  if (endpoints.empty()) return unexpected(Error::network_failure);
  Door door;
  door.destination = endpoints.front();
  door.fallbacks.assign(endpoints.begin() + 1, endpoints.end());
  door.broadcast = broadcast;
  return start(std::move(request), std::move(door));
}

auto HomePairing::pump_once() -> Expected<void, Error> {
  switch (step()) {
    case Step::knocking: {
      const auto answer = knock_.pump_once();
      if (!answer)
        post_failure(stopped_by(answer.error()));
      else if (answer.value()) {
        console_endpoint_ = *answer.value();
        (void)lifecycle_.process(
            home_pairing_lifecycle::KnockAnswered{attempt});
      }
      break;
    }
    case Step::settling:
      if (settled_at_.expired(clock_))
        (void)lifecycle_.process(home_pairing_lifecycle::DelayElapsed{attempt});
      break;
    case Step::connecting:
    case Step::sending:
    case Step::receiving: {
      auto progress = exchange_once();
      if (!progress) {
        if (progress.error() == PairingExchange::Error::refused)
          record_refusal();
        post_failure(stopped_by(progress.error()));
        break;
      }
      follow(std::move(progress.value()));
      break;
    }
    case Step::idle:
      return unexpected(Error::invalid_state);
    case Step::closing:
    case Step::paired:
    case Step::cancelled:
    case Step::failed:
      return outcome();
  }
  run_effects();
  return outcome();
}

auto HomePairing::exchange_once()
    -> Expected<PairingExchange::Progress, PairingExchange::Error> {
  switch (lifecycle_.exchange().value()) {
    case home_pairing_lifecycle::Exchange::connecting:
      return exchange_.await_connection();
    case home_pairing_lifecycle::Exchange::sending_header:
      return exchange_.send_header();
    case home_pairing_lifecycle::Exchange::sending_payload:
      return exchange_.send_payload();
    case home_pairing_lifecycle::Exchange::awaiting_reply_header:
      return exchange_.read_reply_header();
    case home_pairing_lifecycle::Exchange::reading_reply_body:
      return exchange_.read_reply_body();
  }
  std::unreachable();
}

auto HomePairing::cancel() -> Expected<void, Error> {
  const auto now = step();
  if (now == Step::idle) return unexpected(Error::invalid_state);
  if (terminal(now)) return {};
  if (!lifecycle_.process(home_pairing_lifecycle::Cancel{attempt}))
    return unexpected(Error::invalid_state);
  run_effects();
  return step() == Step::failed
             ? unexpected(last_error_.value_or(Error::cleanup_failure))
             : Expected<void, Error>{};
}

auto HomePairing::take_result()
    -> Expected<net::protocol::remote_play::RegisteredConsole, Error> {
  if (step() != Step::paired || !result_)
    return unexpected(Error::invalid_state);
  auto result = std::move(*result_);
  result_.reset();
  return result;
}

void HomePairing::wait(std::chrono::milliseconds timeout) {
  const platform::Pollable* pollable = nullptr;
  auto interest = platform::PollEvent::readable;
  auto bound = std::chrono::nanoseconds{timeout};
  switch (step()) {
    case Step::knocking:
      pollable = knock_.pollable();
      bound = std::min(bound, std::chrono::nanoseconds{
                                  net::protocol::remote_play::timer_tick});
      break;
    case Step::settling:
      bound = std::min(bound,
                       std::chrono::nanoseconds{
                           settled_at_.remaining(timing::Deadline::now(clock_))
                               .value_or(timing::Deadline::Duration::zero())});
      break;
    case Step::connecting:
    case Step::sending:
      pollable = exchange_.pollable();
      interest = platform::PollEvent::writable;
      bound = std::min(bound, std::chrono::nanoseconds{
                                  net::protocol::remote_play::timer_tick});
      break;
    case Step::receiving:
      pollable = exchange_.pollable();
      bound = std::min(bound, std::chrono::nanoseconds{
                                  net::protocol::remote_play::timer_tick});
      break;
    case Step::idle:
    case Step::closing:
    case Step::paired:
    case Step::cancelled:
    case Step::failed:
      break;
  }
  waiter_.wait(pollable, interest, bound);
}

void HomePairing::wake() noexcept { waiter_.wake(); }

auto HomePairing::stopped_by(PairingKnock::Error error) noexcept -> Error {
  return error == PairingKnock::Error::timeout ? Error::timeout
                                               : Error::network_failure;
}

auto HomePairing::stopped_by(PairingExchange::Error error) noexcept -> Error {
  switch (error) {
    case PairingExchange::Error::timeout:
      return Error::timeout;
    case PairingExchange::Error::refused:
      return Error::refused;
    case PairingExchange::Error::crypto_failure:
      return Error::crypto_failure;
    case PairingExchange::Error::invalid_header:
    case PairingExchange::Error::too_large:
    case PairingExchange::Error::truncated:
    case PairingExchange::Error::invalid_payload:
      return Error::invalid_reply;
    case PairingExchange::Error::invalid_argument:
    case PairingExchange::Error::invalid_state:
    case PairingExchange::Error::network_failure:
      break;
  }
  return Error::network_failure;
}

auto HomePairing::reason_for(Error error) noexcept -> PairingFailure::Reason {
  switch (error) {
    case Error::invalid_request:
      return PairingFailure::Reason::malformed;
    case Error::invalid_state:
      return PairingFailure::Reason::busy;
    case Error::crypto_failure:
      return PairingFailure::Reason::unreadable_reply;
    case Error::invalid_reply:
      return PairingFailure::Reason::malformed;
    case Error::refused:
      return PairingFailure::Reason::refused;
    case Error::timeout:
      return PairingFailure::Reason::timed_out;
    case Error::cancelled:
      return PairingFailure::Reason::cancelled;
    case Error::network_failure:
    case Error::cleanup_failure:
      break;
  }
  return PairingFailure::Reason::unreachable;
}

auto HomePairing::ps_reason_text(std::uint32_t reason) noexcept
    -> std::string_view {
  switch (reason) {
    case rp_reason_regist_failed:
      return "Regist failed, probably invalid PIN";
    case rp_reason_invalid_psn_id:
      return "Invalid PSN ID";
    case rp_reason_in_use:
      return "Remote is already in use";
    case rp_reason_crash:
      return "Remote Play on Console crashed";
    case rp_reason_rp_version:
      return "RP-Version mismatch";
    default:
      return "unknown";
  }
}

auto HomePairing::terminal(Step step) noexcept -> bool {
  return step == Step::paired || step == Step::cancelled ||
         step == Step::failed;
}

auto HomePairing::paired(net::protocol::remote_play::RegisteredConsole console,
                         Firmware asked)
    -> Expected<PairedConsole, PairingFailure> {
  const auto hardware_id = HardwareId::from_bytes(console.mac);
  auto keys = PairingKeys::create(
      std::span<const std::byte, PairingKeys::registration_key_size>{
          console.registration_key.data(), console.registration_key.size()},
      std::span<const std::byte, PairingKeys::remote_play_key_size>{
          console.key.data(), console.key.size()},
      console.key_type);
  if (!hardware_id || !keys)
    return unexpected(PairingFailure{PairingFailure::Reason::malformed});
  const auto nickname = std::string_view{
      console.nickname.data(),
      static_cast<std::size_t>(std::ranges::find(console.nickname, '\0') -
                               console.nickname.begin())};
  return PairedConsole{
      *hardware_id, Firmware::from(console.target).value_or(asked),
      ConsoleName::create(std::string{nickname}, ConsoleName::Source::pairing),
      std::move(*keys)};
}

auto HomePairing::prepare(const Request& request) -> Expected<void, Error> {
  if (!PairingExchange::known(target_) || door_.destination.port == 0 ||
      door_.destination.address.is_unspecified())
    return unexpected(Error::invalid_request);
  SecretArray<rp_key_size> ambassador;
  if (!random_.fill_bytes({ambassador.data(), ambassador.size()}))
    return unexpected(Error::crypto_failure);
  auto derived = net::protocol::remote_play::RegistrationMaterial::derive_local(
      target_, {ambassador.data(), ambassador.size()}, request.pin_.value());
  if (!derived) return unexpected(Error::invalid_request);
  material_ = std::move(derived.value());

  auto payload =
      net::protocol::remote_play::RegistrationRequestCodec::encode_payload(
          crypto_, target_, material_,
          request.account_
              ? std::span<const std::byte>{request.account_->data(),
                                           request.account_->size()}
              : std::span<const std::byte>{},
          request.online_id_
              ? std::optional<std::string_view>{*request.online_id_}
              : std::nullopt);
  if (!payload)
    return unexpected(
        payload.error() ==
                net::protocol::remote_play::PayloadError::provider_failure
            ? Error::crypto_failure
            : Error::invalid_request);
  const auto header =
      net::protocol::remote_play::RegistrationRequestCodec::encode_header(
          target_, payload.value().size());
  const auto staged =
      header ? exchange_.stage(std::as_bytes(std::span{header.value()}),
                               payload.value(), target_)
             : Expected<void, PairingExchange::Error>{
                   unexpected(PairingExchange::Error::invalid_argument)};
  if (!staged) return unexpected(Error::invalid_request);
  return {};
}

auto HomePairing::outcome() const -> Expected<void, Error> {
  if (step() == Step::failed)
    return unexpected(last_error_.value_or(Error::network_failure));
  if (step() == Step::cancelled) return unexpected(Error::cancelled);
  return {};
}

auto HomePairing::failed(Error error, const platform::StopToken& stop) const
    -> Unexpected<PairingFailure> {
  if (stop.stop_requested())
    return unexpected(PairingFailure{PairingFailure::Reason::cancelled});
  return unexpected(PairingFailure{reason_for(error), refusal_.http_status,
                                   refusal_.ps_reason});
}

void HomePairing::clear_prepared() noexcept {
  exchange_.unstage();
  material_ = net::protocol::remote_play::RegistrationMaterial{};
}

void HomePairing::write(platform::LogSeverity severity,
                        std::string_view message) noexcept {
  if (log_) log_->write({severity, message});
}

void HomePairing::record_refusal() {
  refusal_.http_status = exchange_.http_status();
  refusal_.ps_reason = exchange_.ps_reason();
  write(platform::LogSeverity::error,
        std::format("Regist received HTTP code {}",
                    refusal_.http_status.value_or(0)));
  if (refusal_.ps_reason)
    write(
        platform::LogSeverity::error,
        std::format("Reported Application Reason: {:#x} ({})",
                    *refusal_.ps_reason, ps_reason_text(*refusal_.ps_reason)));
}

void HomePairing::follow(PairingExchange::Progress progress) {
  std::visit(
      [this]<class Reached>(Reached&& reached) {
        using Kind = std::remove_cvref_t<Reached>;
        if constexpr (std::is_same_v<Kind, PairingExchange::Connected>) {
          (void)lifecycle_.process(home_pairing_lifecycle::Connected{attempt});
        } else if constexpr (std::is_same_v<
                                 Kind, PairingExchange::ConnectionPending>) {
          (void)lifecycle_.process(
              home_pairing_lifecycle::ConnectionPending{attempt});
        } else if constexpr (std::is_same_v<Kind,
                                            PairingExchange::HeaderSent>) {
          (void)lifecycle_.process(home_pairing_lifecycle::HeaderSent{attempt});
        } else if constexpr (std::is_same_v<Kind,
                                            PairingExchange::RequestSent>) {
          (void)lifecycle_.process(
              home_pairing_lifecycle::RequestSent{attempt});
        } else if constexpr (std::is_same_v<
                                 Kind, PairingExchange::ReplyHeaderAccepted>) {
          (void)lifecycle_.process(
              home_pairing_lifecycle::ReplyHeaderAccepted{attempt});
        } else if constexpr (std::is_same_v<Kind, net::protocol::remote_play::
                                                      RegisteredConsole>) {
          result_ = std::forward<Reached>(reached);
          result_->console_pin = console_pin_;
          (void)lifecycle_.process(
              home_pairing_lifecycle::ReplyDecoded{attempt});
        }
      },
      std::move(progress));
}

void HomePairing::give_up() {
  post_failure(Error::timeout);
  run_effects();
}

void HomePairing::post_failure(Error error) {
  (void)lifecycle_.process(home_pairing_lifecycle::Failed{attempt, error});
}

void HomePairing::run_effects() {
  while (!context_.effects.empty()) {
    const auto effect = std::move(context_.effects.front());
    context_.effects.pop_front();
    std::visit([this](const auto& each) { handle(each); }, effect);
  }
}

void HomePairing::handle(const home_pairing_lifecycle::BeginKnock&) {
  PairingKnock::Door door;
  door.generation =
      target_ == net::protocol::remote_play::RegistrationTarget::ps5_1
          ? Generation::ps5
          : Generation::ps4;
  door.destination = door_.destination;
  door.fallbacks = door_.fallbacks;
  door.broadcast = door_.broadcast;
  const auto started = knock_.start(std::move(door));
  if (!started) post_failure(stopped_by(started.error()));
}

void HomePairing::handle(const home_pairing_lifecycle::WaitAfterKnock&) {
  settled_at_ = timing::Deadline::after(
      clock_,
      timing::Deadline::Duration{net::protocol::remote_play::search_delay});
}

void HomePairing::handle(const home_pairing_lifecycle::BeginConnect&) {
  auto started = exchange_.begin(console_endpoint_, std::move(material_));
  if (!started) {
    post_failure(stopped_by(started.error()));
    return;
  }
  follow(std::move(started.value()));
}

void HomePairing::handle(const home_pairing_lifecycle::Cleanup&) {
  const auto knock_closed = knock_.cancel();
  const auto exchange_closed = exchange_.cancel();
  clear_prepared();
  const bool success = knock_closed && exchange_closed;
  (void)lifecycle_.process(home_pairing_lifecycle::Closed{attempt, success});
}

void HomePairing::handle(const home_pairing_lifecycle::PublishPaired&) {}

void HomePairing::handle(const home_pairing_lifecycle::PublishCancelled&) {
  last_error_ = Error::cancelled;
}

void HomePairing::handle(const home_pairing_lifecycle::PublishFailed& effect) {
  last_error_ = effect.error;
}

}  // namespace libchiaking::consoles
