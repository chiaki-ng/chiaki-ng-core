#include "src/core/play/adapters/links/cloud/server_reservation.hpp"

#include "src/core/play/adapters/links/cloud/datacenter_survey.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace libchiaking::play {

struct ServerReservation::Traced {
  TraceCode kind;
  Error error;
};

auto ServerReservation::poll_failure_of(const Error& error) noexcept
    -> ReservationPollFailure {
  if (error.is_cancelled()) return ReservationPollFailure::cancelled;
  if (error.reason() == Error::Reason::transport)
    return ReservationPollFailure::network;
  return ReservationPollFailure::refused;
}

auto ServerReservation::generation_of(psn::StreamingService service) noexcept
    -> catalog::ServiceGeneration {
  return service == psn::StreamingService::legacy_cloud_stream
             ? catalog::ServiceGeneration::old_service
             : catalog::ServiceGeneration::ps5_cloud;
}

auto ServerReservation::trace_code(diagnostics::Operation operation,
                                   const Error& error) noexcept
    -> std::uint64_t {
  using diagnostics::Operation;
  using Reason = Error::Reason;
  const bool invalid = error.is_invalid_request();
  const bool cancelled = error.is_cancelled();
  const bool transport = error.reason() == Reason::transport;
  const bool session_key = error.reason() == Reason::invalid_session_key;
  switch (operation) {
    case Operation::cloud_authorize:
      if (invalid) return 0;
      if (cancelled) return 1;
      if (transport) return 2;
      if (error.reason() == Reason::psplus_rejected) return 3;
      if (error.reason() == Reason::entitlement_retry) return 4;
      return session_key ? 6 : 5;
    case Operation::cloud_lock:
      if (invalid) return 0;
      return cancelled ? 1 : 2;
    case Operation::lock_poll:
    case Operation::allocation_poll:
      if (invalid) return 0;
      if (cancelled) return 1;
      if (transport) return 2;
      if (session_key) return 4;
      if (operation == Operation::allocation_poll &&
          error.reason() == Reason::malformed)
        return 5;
      if (operation == Operation::allocation_poll &&
          error.reason() == Reason::missing_launch_slot)
        return 6;
      return 3;
    default:
      return error.detail();
  }
}

template <class T, class Step>
auto ServerReservation::observed(const diagnostics::Trace* trace,
                                 diagnostics::Operation operation, Step&& step)
    -> Expected<T, Error> {
  auto result =
      diagnostics::observe(trace, operation, [&]() -> Expected<T, Traced> {
        auto outcome = step();
        if (!outcome)
          return unexpected(Traced{
              static_cast<TraceCode>(trace_code(operation, outcome.error())),
              outcome.error()});
        if constexpr (std::is_void_v<T>)
          return {};
        else
          return std::move(outcome).value();
      });
  if (!result) return unexpected(result.error().error);
  if constexpr (std::is_void_v<T>)
    return {};
  else
    return std::move(result).value();
}

auto ServerReservation::best_measured_rtt(const std::vector<Ping>& rows)
    -> std::optional<int> {
  std::optional<int> best;
  for (const auto& row : rows)
    if (row.measured) best = best ? std::min(*best, row.rtt_ms) : row.rtt_ms;
  return best;
}

using diagnostics::Operation;
using Reason = psn::gk::Error::Reason;
using Stage = psn::gk::Error::Stage;

ServerReservation::ServerReservation(Client& client, Hooks hooks,
                                     LaunchSpec spec,
                                     psn::StreamingPlatform platform,
                                     ClientIds ids, ReservationId id,
                                     ConfigKey key) noexcept
    : client_(&client),
      waiter_(&hooks.waiter),
      trace_(hooks.trace),
      observer_(&hooks.observer),
      report_(hooks.observer, generation_of(spec.service())),
      spec_(std::move(spec)),
      platform_(platform),
      ids_(std::move(ids)),
      id_(std::move(id)),
      key_(std::move(key)) {}

auto ServerReservation::settle(Client& client, Hooks hooks,
                               psn::StreamingService service,
                               const users::CloudRelaunch& relaunch)
    -> Expected<void, Error> {
  const auto left = relaunch.remaining();
  if (left <= std::chrono::milliseconds{0}) return {};
  const auto& stop = client.stop_token();
  const auto whole = std::chrono::ceil<std::chrono::seconds>(left).count();
  const LaunchReport progress{hooks.observer, generation_of(service)};
  progress.say(LaunchActivity::reserving_server,
               net::protocol::remote_play::FailureStage::cloud_lock,
               settling_text(whole), progress_text_limit,
               LaunchReport::waiting(progress::WaitReason::recent_cloud_session,
                                     0, static_cast<int>(whole)));
  if (stop.stop_requested() ||
      !hooks.waiter.wait_milliseconds(static_cast<int>(left.count()), stop) ||
      stop.stop_requested())
    return unexpected(Error{Stage::lock, Reason::cancelled});
  return {};
}

auto ServerReservation::settling_text(std::int64_t seconds) -> std::string {
  return std::format("Waiting {} {} since last cloud stream", seconds,
                     seconds == 1 ? "second" : "seconds");
}

auto ServerReservation::open(Client& client, Hooks hooks, LaunchSpec spec,
                             psn::StreamingPlatform platform)
    -> Expected<ServerReservation, Error> {
  const auto service = spec.service();
  const auto& stop = client.stop_token();
  const auto cancelled = [](Stage stage) {
    return unexpected(Error{stage, Reason::cancelled});
  };
  const LaunchReport progress{hooks.observer, generation_of(service)};
  const auto reserving = LaunchActivity::reserving_server;

  progress.step(reserving,
                net::protocol::remote_play::FailureStage::cloud_client_ids,
                "Getting Client IDs", 1, progress_steps);
  auto ids = observed<ClientIds>(hooks.trace, Operation::cloud_client_ids,
                                 [&]() -> Expected<ClientIds, Error> {
                                   if (client.stop_requested())
                                     return cancelled(Stage::client_ids);
                                   return client.client_ids(service, platform);
                                 });
  if (!ids) return unexpected(ids.error());
  if (stop.stop_requested()) return cancelled(Stage::client_ids);

  progress.step(reserving,
                net::protocol::remote_play::FailureStage::cloud_config,
                "Getting Configuration", 2, progress_steps);
  auto config = observed<ConfigKey>(hooks.trace, Operation::cloud_config,
                                    [&]() -> Expected<ConfigKey, Error> {
                                      if (client.stop_requested())
                                        return cancelled(Stage::config);
                                      return client.config(service);
                                    });
  if (!config) return unexpected(config.error());
  if (stop.stop_requested()) return cancelled(Stage::config);

  progress.step(reserving,
                net::protocol::remote_play::FailureStage::cloud_start,
                "Starting Session", 3, progress_steps);
  auto started = observed<psn::gk::Started>(
      hooks.trace, Operation::cloud_start,
      [&]() -> Expected<psn::gk::Started, Error> {
        if (client.stop_requested()) return cancelled(Stage::start);
        return client.start(spec.json(), config.value());
      });
  if (!started) return unexpected(started.error());
  auto key = started.value().rotated_key
                 ? std::move(*started.value().rotated_key)
                 : std::move(config.value());
  return ServerReservation{client,
                           hooks,
                           std::move(spec),
                           platform,
                           std::move(ids.value()),
                           std::move(started.value().session_id),
                           std::move(key)};
}

auto ServerReservation::open_locked(Client& client, Hooks hooks,
                                    const LaunchSpec& spec,
                                    psn::StreamingPlatform platform,
                                    users::PlayCalls& user,
                                    net::http::Client& http)
    -> Expected<ServerReservation, Error> {
  ReservationRestart restart;
  for (;;) {
    auto opened = open(client, hooks, spec, platform);
    if (!opened) return unexpected(opened.error());
    auto granted = opened.value().sign_in(user, http);
    if (granted) granted = opened.value().lock();
    if (granted) return std::move(opened.value());
    if (!refused_lock(granted.error())) return unexpected(granted.error());
    const auto delay = restart.on_refused_lock();
    if (!delay) return unexpected(granted.error());
    if (!rest_before_restart(client, hooks, spec.service(), restart.restarts(),
                             *delay))
      return unexpected(Error{Stage::lock, Reason::cancelled});
  }
}

auto ServerReservation::refused_lock(const Error& error) noexcept -> bool {
  return error.stage() == Stage::lock && error.reason() == Reason::rejected &&
         error.http_status() == lock_refused_status;
}

auto ServerReservation::rest_before_restart(Client& client, Hooks hooks,
                                            psn::StreamingService service,
                                            int restart, int seconds) -> bool {
  const LaunchReport progress{hooks.observer, generation_of(service)};
  progress.say(
      LaunchActivity::reserving_server,
      net::protocol::remote_play::FailureStage::cloud_lock,
      std::format("Lock refused, starting a new session - Attempt {}", restart),
      progress_text_limit,
      LaunchReport::waiting(progress::WaitReason::connection_retry, restart,
                            seconds));
  const auto& stop = client.stop_token();
  return !stop.stop_requested() &&
         hooks.waiter.wait_milliseconds(seconds * 1000, stop) &&
         !stop.stop_requested();
}

auto ServerReservation::refused(
    Error::Stage stage, const users::AuthorisationRefusal& refusal) noexcept
    -> Error {
  if (const auto& error = refusal.authorisation()) return Error{stage, *error};
  return Error{stage, Error::Reason::invalid_npsso};
}

auto ServerReservation::sign_in(users::PlayCalls& user, net::http::Client& http)
    -> Expected<void, Error> {
  auto authorized = observed<SecretString>(
      trace_, Operation::cloud_oauth, [&]() -> Expected<SecretString, Error> {
        if (client_->stop_requested())
          return unexpected(Error{Stage::oauth, Reason::cancelled});
        auto codes = user.cloud_streaming_codes(
            http, spec_.service(), platform_, ids_,
            [this](psn::auth::GkStep step) {
              if (step == psn::auth::GkStep::client)
                announce(LaunchActivity::reserving_server,
                         net::protocol::remote_play::FailureStage::cloud_oauth,
                         "Getting Tokens", 4);
              else
                announce(LaunchActivity::reserving_server,
                         net::protocol::remote_play::FailureStage::cloud_oauth,
                         "Getting Server Tokens", 5);
            });
        if (!codes) return unexpected(refused(Stage::oauth, codes.error()));
        return authorized_from(codes.value());
      });
  if (!authorized) return unexpected(authorized.error());
  auto accepted = authorize_with(std::move(authorized.value()));
  if (!accepted && accepted.error().reason() == Reason::psplus_rejected)
    user.membership_rejected_by_service();
  return accepted;
}

auto ServerReservation::authorize(const psn::auth::GkCodes& codes)
    -> Expected<void, Error> {
  auto authorized = authorized_from(codes);
  if (!authorized) return unexpected(authorized.error());
  return authorize_with(std::move(authorized.value()));
}

auto ServerReservation::authorized_from(const psn::auth::GkCodes& codes) const
    -> Expected<SecretString, Error> {
  auto authorized = spec_.authorized(codes);
  if (!authorized) return unexpected(Error{Stage::oauth, Reason::invalid_spec});
  return std::move(authorized.value());
}

auto ServerReservation::authorize_with(SecretString authorized)
    -> Expected<void, Error> {
  authorized_ = std::move(authorized);
  if (client_->stop_requested())
    return unexpected(Error{Stage::authorize, Reason::cancelled});
  announce(LaunchActivity::reserving_server,
           net::protocol::remote_play::FailureStage::cloud_authorize,
           "Authorizing Session", 6);
  auto accepted = observed<psn::gk::Authorized>(
      trace_, Operation::cloud_authorize,
      [&]() -> Expected<psn::gk::Authorized, Error> {
        if (client_->stop_requested())
          return unexpected(Error{Stage::authorize, Reason::cancelled});
        return client_->authorize(id_, key_, authorized_.view());
      });
  if (!accepted) return unexpected(accepted.error());
  rotate(std::move(accepted.value().rotated_key));
  return {};
}

void ServerReservation::announce_lock_wait(bool network, int attempt,
                                           int delay_seconds,
                                           std::string_view event_name) const {
  if (trace_)
    trace_->decision(network ? diagnostics::Trace::Decision::lock_network_retry
                             : diagnostics::Trace::Decision::lock_waiting,
                     static_cast<std::uint64_t>(delay_seconds));
  if (network) {
    report_.say(LaunchActivity::reserving_server,
                net::protocol::remote_play::FailureStage::cloud_lock,
                "Locking Session - reconnecting...",
                LaunchReport::waiting(progress::WaitReason::network_unreachable,
                                      0, delay_seconds));
    return;
  }
  const auto attempt_text = std::to_string(attempt);
  report_.say(
      LaunchActivity::reserving_server,
      net::protocol::remote_play::FailureStage::cloud_lock,
      event_name.empty() ? "Closing old session - Attempt " + attempt_text
                         : "Closing old session (" + std::string{event_name} +
                               ") - Attempt " + attempt_text,
      progress_text_limit,
      LaunchReport::waiting(progress::WaitReason::previous_session_closing,
                            attempt, delay_seconds));
}

void ServerReservation::announce_allocation_wait(
    bool network, int attempt, int delay_seconds,
    const psn::gk::AllocationProgress* waiting) const {
  if (trace_)
    trace_->decision(
        network ? diagnostics::Trace::Decision::allocation_network_retry
                : diagnostics::Trace::Decision::allocation_waiting,
        static_cast<std::uint64_t>(delay_seconds));
  if (network)
    report_.say(LaunchActivity::allocating_server,
                net::protocol::remote_play::FailureStage::cloud_allocate,
                "Allocating streaming slot - reconnecting...",
                LaunchReport::waiting(progress::WaitReason::network_unreachable,
                                      0, delay_seconds));
  else
    announce_waiting(*waiting, attempt, delay_seconds);
}

auto ServerReservation::lock() -> Expected<void, Error> {
  return observed<void>(
      trace_, Operation::cloud_lock, [&]() -> Expected<void, Error> {
        const auto& stop = client_->stop_token();
        const Error cancelled{Stage::lock, Reason::cancelled};
        announce(LaunchActivity::reserving_server,
                 net::protocol::remote_play::FailureStage::cloud_lock,
                 "Locking Session", 7);
        ReservationLockRetry retry;
        for (;;) {
          if (stop.stop_requested()) return unexpected(cancelled);
          auto attempt = observed<psn::gk::LockAttempt>(
              trace_, Operation::lock_poll,
              [&]() -> Expected<psn::gk::LockAttempt, Error> {
                if (client_->stop_requested()) return unexpected(cancelled);
                return client_->lock(id_, key_, authorized_.view());
              });
          if (!attempt && attempt.error().is_invalid_request())
            return unexpected(attempt.error());
          ReservationLockRetry::Decision decision{
              ReservationLockRetry::Step::failed};
          if (attempt) {
            rotate(std::move(attempt.value().rotated_key));
            const auto& response = attempt.value().response;
            decision = retry.on_reply(response.acquired,
                                      response.poll_seconds > 0
                                          ? response.poll_seconds
                                          : psn::gk::default_lock_poll_seconds);
          } else {
            decision = retry.on_failure(poll_failure_of(attempt.error()));
          }
          switch (decision.step) {
            case ReservationLockRetry::Step::acquired:
              lock_key_ = key_.clone();
              return {};
            case ReservationLockRetry::Step::wait_poll:
            case ReservationLockRetry::Step::wait_network:
              announce_lock_wait(
                  decision.step == ReservationLockRetry::Step::wait_network,
                  retry.attempt(), decision.delay_seconds,
                  attempt ? std::string_view{attempt.value().event_name}
                          : std::string_view{});
              if (!pause(decision.delay_seconds)) return unexpected(cancelled);
              break;
            case ReservationLockRetry::Step::failed:
              return unexpected(
                  attempt ? Error{Stage::lock, Reason::lock_unavailable}
                          : attempt.error());
            case ReservationLockRetry::Step::cancelled:
              return unexpected(cancelled);
          }
        }
      });
}

auto ServerReservation::datacenters()
    -> Expected<std::vector<Datacenter>, Error> {
  if (client_->stop_requested())
    return unexpected(Error{Stage::datacenters, Reason::cancelled});
  auto fetched = client_->datacenters(id_, key_, authorized_.view());
  if (!fetched) return unexpected(fetched.error());
  rotate(std::move(fetched.value().rotated_key));
  return std::move(fetched.value().rows);
}

auto ServerReservation::select(const Chosen& choice) -> Expected<Ping, Error> {
  if (client_->stop_requested())
    return unexpected(Error{Stage::select, Reason::cancelled});
  auto result =
      client_->select(id_, key_, authorized_.view(), choice.ping_results,
                      choice.selected.selected_port);
  if (!result) return unexpected(result.error());
  auto selected = choice.selected;
  selected.selected_port = result.value().port;
  rotate(std::move(result.value().rotated_key));
  return selected;
}

auto ServerReservation::choose_datacenter(const Survey& survey,
                                          std::string_view forced,
                                          const std::vector<Ping>& prior)
    -> Expected<Chosen, Error> {
  return observed<Chosen>(
      trace_, Operation::cloud_datacenter, [&]() -> Expected<Chosen, Error> {
        if (client_->stop_requested())
          return unexpected(Error{Stage::datacenters, Reason::cancelled});
        announce(LaunchActivity::measuring_datacenters,
                 net::protocol::remote_play::FailureStage::cloud_datacenter,
                 "Getting Datacenters", 8);
        auto fetched = datacenters();
        if (!fetched) return unexpected(fetched.error());

        const bool pick_at_launch = observer_->selects_datacenter_at_launch();
        if (pick_at_launch) forced = {};
        std::vector<Ping> completed;
        if (forced.empty() || forced == DatacenterName::automatic) {
          announce(LaunchActivity::measuring_datacenters,
                   net::protocol::remote_play::FailureStage::cloud_datacenter,
                   "Pinging Datacenters", 8);
          const SecretString unlocked{};
          auto measured = survey(fetched.value(), spec_.service(),
                                 lock_key_ ? lock_key_->secret() : unlocked);
          if (!measured) return unexpected(measured.error());
          completed = std::move(measured.value());
        }
        return pick(fetched.value(), forced, std::move(completed), prior,
                    pick_at_launch);
      });
}

auto ServerReservation::choose_datacenter(Probes probes,
                                          std::string_view forced,
                                          const std::vector<Ping>& prior)
    -> Expected<Chosen, Error> {
  const Survey survey =
      [&](const std::vector<Datacenter>& rows, psn::StreamingService service,
          const SecretString& lock_key) -> Expected<std::vector<Ping>, Error> {
    const auto failed = [](DatacenterSurvey::Error error) {
      return unexpected(
          Error{Stage::ping, error == DatacenterSurvey::Error::cancelled
                                 ? Reason::cancelled
                                 : Reason::probe_failed});
    };
    const auto& stop = client_->stop_token();
    auto started = DatacenterSurvey::start(
        {probes.network, probes.clock, probes.threading, probes.random}, rows,
        service == psn::StreamingService::cloud_stream
            ? PingService::cloud_stream
            : PingService::legacy_cloud_stream,
        lock_key, stop);
    if (!started) return failed(started.error());
    auto measured = started.value().run(
        probes.waiter, stop, [this](std::size_t done, std::size_t total) {
          report_.say(
              LaunchActivity::measuring_datacenters,
              net::protocol::remote_play::FailureStage::cloud_datacenter,
              "Pinging Datacenters (" + std::to_string(done) + " of " +
                  std::to_string(total) + ")",
              datacenter_progress_text_limit,
              LaunchReport::measured(done, total, 8, progress_steps));
        });
    if (!measured) return failed(measured.error());
    return std::move(measured.value().rows);
  };
  return choose_datacenter(survey, forced, prior);
}

auto ServerReservation::pick(const std::vector<Datacenter>& rows,
                             std::string_view forced,
                             std::vector<Ping> completed,
                             const std::vector<Ping>& prior,
                             bool pick_at_launch) -> Expected<Chosen, Error> {
  const auto& stop = client_->stop_token();
  const Error cancelled{Stage::select, Reason::cancelled};
  const Error unavailable{Stage::select, Reason::datacenter_unavailable};
  if (stop.stop_requested()) return unexpected(cancelled);
  const DatacenterRanking picker{rows, forced};
  auto candidates = picker.choose(std::move(completed), prior);
  if (!candidates) return unexpected(unavailable);
  auto choice = std::move(*candidates);
  auto best_rtt_ms = best_measured_rtt(choice.ping_results);
  bool forced_pick = picker.forced();
  if (pick_at_launch) {
    const DatacenterRanking launch{rows};
    observer_->offer_datacenters(launch.offer());
    observer_->on_datacenters(launch.picker_rows(choice.ping_results, {}),
                              choice.ping_results);
    auto waiting = LaunchReport::counted(9, progress_steps);
    waiting.wait = progress::Wait{progress::WaitReason::datacenter_choice,
                                  observer_->datacenter_choice_wait()};
    report_.say(LaunchActivity::waiting_for_datacenter_choice,
                net::protocol::remote_play::FailureStage::cloud_datacenter,
                LaunchReport::step_text("Waiting for Datacenter Selection", 9,
                                        progress_steps),
                waiting);
    auto picked = observer_->await_datacenter_choice(stop);
    if (picked.kind == LaunchChoice::Kind::cancelled || stop.stop_requested())
      return unexpected(cancelled);
    if (picked.kind == LaunchChoice::Kind::named) {
      auto applied = launch.pick(picked.name, std::move(choice));
      if (!applied) return unexpected(unavailable);
      choice = std::move(*applied);
      forced_pick = true;
    }
  } else {
    observer_->on_datacenters(choice.picker_rows, choice.ping_results);
  }
  if (!choice.selected)
    return unexpected(Error::no_reachable_datacenter(best_rtt_ms));
  observer_->on_datacenter_selected(*choice.selected, forced_pick);
  report_.step(LaunchActivity::measuring_datacenters,
               net::protocol::remote_play::FailureStage::cloud_datacenter,
               "Selecting Datacenter (" + choice.selected->name + ")", 9,
               progress_steps, datacenter_progress_text_limit);

  Chosen chosen{std::move(*choice.selected), std::move(choice.ping_results),
                std::move(choice.picker_rows)};
  auto selected = select(chosen);
  if (!selected) return unexpected(selected.error());
  chosen.selected = std::move(selected.value());
  return chosen;
}

auto ServerReservation::allocate(const Ping& selected, int bitrate_kbps)
    -> Expected<Allocation, Error> {
  return observed<Allocation>(
      trace_, Operation::cloud_allocate, [&]() -> Expected<Allocation, Error> {
        const auto& stop = client_->stop_token();
        const Error cancelled{Stage::allocate, Reason::cancelled};
        announce(LaunchActivity::allocating_server,
                 net::protocol::remote_play::FailureStage::cloud_allocate,
                 "Allocating Streaming Slot", 10);
        ReservationAllocateRetry retry;
        for (;;) {
          if (stop.stop_requested()) return unexpected(cancelled);
          auto attempt = observed<psn::gk::AllocateState>(
              trace_, Operation::allocation_poll,
              [&]() -> Expected<psn::gk::AllocateState, Error> {
                if (client_->stop_requested()) return unexpected(cancelled);
                return client_->allocate(id_, key_, authorized_.view(),
                                         selected, selected.selected_port,
                                         bitrate_kbps);
              });
          if (!attempt && attempt.error().is_invalid_request())
            return unexpected(attempt.error());
          ReservationAllocateRetry::Decision decision{
              ReservationAllocateRetry::Step::failed};
          const psn::gk::AllocationProgress* waiting = nullptr;
          if (attempt) {
            rotate(std::move(attempt.value().rotated_key));
            if (auto* ready = std::get_if<Allocation>(&attempt.value().outcome))
              return std::move(*ready);
            waiting = std::get_if<psn::gk::AllocationProgress>(
                &attempt.value().outcome);
            decision = retry.on_progress(waiting->queued || waiting->migrating,
                                         waiting->poll_frequency,
                                         waiting->wait_time_estimate);
          } else {
            decision = retry.on_failure(poll_failure_of(attempt.error()));
          }
          switch (decision.step) {
            case ReservationAllocateRetry::Step::wait_poll:
            case ReservationAllocateRetry::Step::wait_network:
              announce_allocation_wait(
                  decision.step == ReservationAllocateRetry::Step::wait_network,
                  retry.attempt(), decision.delay_seconds, waiting);
              if (!pause(decision.delay_seconds)) return unexpected(cancelled);
              break;
            case ReservationAllocateRetry::Step::network_failed:
              return unexpected(
                  Error{Stage::allocate, Reason::network_exhausted});
            case ReservationAllocateRetry::Step::timed_out:
              return unexpected(Error{
                  Stage::allocate,
                  attempt ? Reason::timed_out : Reason::timed_out_unreachable});
            case ReservationAllocateRetry::Step::failed:
              return unexpected(attempt
                                    ? Error{Stage::allocate, Reason::malformed}
                                    : attempt.error());
            case ReservationAllocateRetry::Step::cancelled:
              return unexpected(cancelled);
          }
        }
      });
}

auto ServerReservation::provision(
    Probes probes, std::string_view forced, const std::vector<Ping>& prior,
    int bitrate_kbps, std::optional<std::uint32_t> measure_up_to_kbps)
    -> Expected<ServerSlot, Error> {
  auto chosen = choose_datacenter(probes, forced, prior);
  if (!chosen) return unexpected(chosen.error());
  auto reported_kbps = bitrate_kbps;
  if (measure_up_to_kbps) {
    const auto& selected = chosen.value().selected;
    if (const auto cached = observer_->cached_bandwidth(selected)) {
      reported_kbps = cached->kbps;
      auto counts = LaunchReport::counted(9, progress_steps);
      counts.bandwidth_kbps = static_cast<std::uint32_t>(cached->kbps);
      counts.bandwidth_age = cached->age;
      report_.say(LaunchActivity::measuring_bandwidth,
                  net::protocol::remote_play::FailureStage::cloud_datacenter,
                  "Reusing Bandwidth", counts);
      if (trace_)
        trace_->record(diagnostics::Operation::cloud_bandwidth,
                       diagnostics::Outcome::observed,
                       static_cast<std::uint64_t>(cached->kbps));
    } else {
      const auto began_us = probes.clock.now_microseconds();
      if (trace_)
        trace_->record(diagnostics::Operation::cloud_bandwidth,
                       diagnostics::Outcome::begin, *measure_up_to_kbps);
      const auto measured =
          measure_bandwidth(probes, selected, *measure_up_to_kbps);
      if (measured) {
        reported_kbps = *measured;
        observer_->on_bandwidth_measured(selected, *measured);
        auto counts = LaunchReport::counted(9, progress_steps);
        counts.bandwidth_kbps = static_cast<std::uint32_t>(*measured);
        report_.say(LaunchActivity::measuring_bandwidth,
                    net::protocol::remote_play::FailureStage::cloud_datacenter,
                    "Measured Bandwidth", counts);
      }
      if (trace_)
        trace_->record(diagnostics::Operation::cloud_bandwidth,
                       measured ? diagnostics::Outcome::succeeded
                                : diagnostics::Outcome::failed,
                       static_cast<std::uint64_t>(reported_kbps),
                       probes.clock.now_microseconds() - began_us);
    }
  }
  auto allocation = allocate(chosen.value().selected, reported_kbps);
  if (!allocation) return unexpected(allocation.error());
  return ServerSlot{std::move(chosen.value().selected),
                    std::move(chosen.value().picker_rows),
                    std::move(allocation.value()),
                    spec_.service(),
                    platform_,
                    {}};
}

auto ServerReservation::bandwidth_test_kbps(int height) noexcept
    -> std::uint32_t {
  if (height > 1440) return 55000;
  if (height > 1080) return 45000;
  return 20000;
}

auto ServerReservation::measure_bandwidth(Probes probes, const Ping& selected,
                                          std::uint32_t target_kbps)
    -> std::optional<int> {
  if (!lock_key_ || client_->stop_requested()) return std::nullopt;
  report_.say(LaunchActivity::measuring_bandwidth,
              net::protocol::remote_play::FailureStage::cloud_datacenter,
              "Measuring Bandwidth", LaunchReport::counted(9, progress_steps));
  net::protocol::senkusha::BandwidthPolicy policy;
  policy.target_kbps = target_kbps;
  const Datacenter datacenter{selected.name, selected.public_ip, selected.port,
                              selected.max_bandwidth};
  const auto& stop = client_->stop_token();
  auto started = DatacenterSurvey::start(
      {probes.network, probes.clock, probes.threading, probes.random},
      {datacenter},
      spec_.service() == psn::StreamingService::cloud_stream
          ? PingService::cloud_stream
          : PingService::legacy_cloud_stream,
      lock_key_->secret(), stop, policy);
  if (!started) return std::nullopt;
  auto measured = started.value().run(probes.waiter, stop);
  if (!measured || measured.value().rows.empty() ||
      measured.value().rows.front().bandwidth_kbps <= 0)
    return std::nullopt;
  return measured.value().rows.front().bandwidth_kbps;
}

auto ServerReservation::pause(int seconds) -> bool {
  const auto& stop = client_->stop_token();
  const auto milliseconds = std::min<std::int64_t>(
      std::int64_t{seconds} * 1000, std::numeric_limits<int>::max());
  return !stop.stop_requested() &&
         waiter_->wait_milliseconds(static_cast<int>(milliseconds), stop) &&
         !stop.stop_requested();
}

void ServerReservation::rotate(std::optional<ConfigKey> rotated) {
  if (rotated) key_ = std::move(*rotated);
}

void ServerReservation::announce(LaunchActivity activity,
                                 net::protocol::remote_play::FailureStage stage,
                                 std::string_view label, int step) const {
  report_.step(activity, stage, label, step, progress_steps);
}

void ServerReservation::announce_waiting(
    const psn::gk::AllocationProgress& waiting, int attempt,
    int delay_seconds) const {
  const auto attempt_text = std::to_string(attempt);
  auto counts = LaunchReport::waiting(waiting.migrating
                                          ? progress::WaitReason::data_migration
                                          : progress::WaitReason::server_queue,
                                      attempt, delay_seconds);
  if (waiting.queued && !waiting.migrating && waiting.wait_time_estimate > 0) {
    counts.wait_time_estimate_seconds =
        static_cast<std::uint32_t>(waiting.wait_time_estimate);
  }
  if (waiting.migrating) {
    counts.migration_percent =
        static_cast<std::uint32_t>(std::clamp<std::int32_t>(
            waiting.migration_percentage, 0, LaunchProgress::whole_migration));
    report_.say(LaunchActivity::migrating_data,
                net::protocol::remote_play::FailureStage::cloud_allocate,
                "Migrating data (" +
                    std::to_string(waiting.migration_percentage) +
                    "%) - Attempt " + attempt_text,
                progress_text_limit, counts);
    return;
  }
  if (waiting.queue_position >= 0) {
    counts.queue_position = static_cast<std::uint32_t>(waiting.queue_position);
    report_.say(LaunchActivity::allocating_server,
                net::protocol::remote_play::FailureStage::cloud_allocate,
                "Allocating streaming slot - Queue position: " +
                    std::to_string(waiting.queue_position) + " - Attempt " +
                    attempt_text,
                progress_text_limit, counts);
    return;
  }
  report_.say(LaunchActivity::allocating_server,
              net::protocol::remote_play::FailureStage::cloud_allocate,
              "Allocating streaming slot - Attempt " + attempt_text,
              progress_text_limit, counts);
}

}  // namespace libchiaking::play
