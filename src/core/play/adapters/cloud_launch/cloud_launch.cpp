#include "libchiaking/detail/play/cloud_launch.hpp"

#include "src/core/net/protocol/takion/profile.hpp"
#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/play/adapters/cloud_launch/cloud_launch_core.hpp"
#include "src/core/play/adapters/links/cloud/server_reservation.hpp"
#include "src/core/users/ports/play_calls.hpp"
#include "src/core/users/server_time.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <magic_enum/magic_enum.hpp>

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace libchiaking::play {

struct CloudLaunch::Core::TracedIdentity {
  TraceCode kind;
  psn::gk::Error error;
};

auto CloudLaunch::service() const noexcept -> catalog::ServiceGeneration {
  return core_->service();
}

auto CloudLaunch::product() const noexcept -> const catalog::ProductId& {
  return core_->product();
}

auto CloudLaunch::progress() const -> LaunchProgress {
  return core_->progress();
}

auto CloudLaunch::offer() const -> std::optional<DatacenterOffer> {
  return core_->offer();
}

auto CloudLaunch::choose_datacenter(const DatacenterName& name)
    -> Expected<void, DatacenterRefusal> {
  return core_->choose_datacenter(name);
}

auto CloudLaunch::choose_lowest_latency() -> Expected<void, DatacenterRefusal> {
  return core_->choose_lowest_latency();
}

auto CloudLaunch::chosen() const -> std::optional<DatacenterChoice> {
  return core_->chosen();
}

auto CloudLaunch::measurements() const -> std::vector<DatacenterMeasurement> {
  return core_->measurements();
}

auto CloudLaunch::refusal() const -> std::optional<LaunchRefused> {
  return core_->refusal();
}

CloudLaunch::Core::Core(Doors doors, Request request, Listener& listener)
    : doors_(doors),
      request_(std::move(request)),
      listener_(listener),
      mutex_(doors_.threading),
      service_(expected_service(&doors_.clearance, request_.user.get(),
                                request_.product)),
      progress_(service_),
      pick_(doors_.threading) {}

CloudLaunch::Core::~Core() = default;

auto CloudLaunch::Core::streaming_service(
    catalog::ServiceGeneration service) noexcept -> psn::StreamingService {
  return service == catalog::ServiceGeneration::old_service
             ? psn::StreamingService::legacy_cloud_stream
             : psn::StreamingService::cloud_stream;
}

auto CloudLaunch::Core::expected_service(TitleClearance* clearance,
                                         users::PsUser* user,
                                         const catalog::ProductId& product)
    -> catalog::ServiceGeneration {
  if (clearance != nullptr && user != nullptr && user->has_psn_account()) {
    const auto held = clearance->held(*user, product);
    if (const auto& cleared = held.cleared_launch()) return cleared->service;
  }
  const auto platform = product.platform();
  return platform && *platform != catalog::Platform::ps5
             ? catalog::ServiceGeneration::old_service
             : catalog::ServiceGeneration::ps5_cloud;
}

auto CloudLaunch::Core::service() const noexcept -> catalog::ServiceGeneration {
  const platform::Lock lock{mutex_};
  return service_;
}

auto CloudLaunch::Core::progress() const -> LaunchProgress {
  const platform::Lock lock{mutex_};
  return progress_;
}

auto CloudLaunch::Core::offer() const -> std::optional<DatacenterOffer> {
  const platform::Lock lock{mutex_};
  return offer_;
}

auto CloudLaunch::Core::chosen() const -> std::optional<DatacenterChoice> {
  const platform::Lock lock{mutex_};
  return chosen_;
}

auto CloudLaunch::Core::measurements() const
    -> std::vector<DatacenterMeasurement> {
  const platform::Lock lock{mutex_};
  return measurements_;
}

auto CloudLaunch::Core::refusal() const -> std::optional<LaunchRefused> {
  const platform::Lock lock{mutex_};
  return refusal_;
}

auto CloudLaunch::Core::choose_datacenter(const DatacenterName& name)
    -> Expected<void, DatacenterRefusal> {
  return choose_by_name(name.text());
}

auto CloudLaunch::Core::choose_lowest_latency()
    -> Expected<void, DatacenterRefusal> {
  return choose_by_name(automatic_choice);
}

auto CloudLaunch::Core::choose_by_name(std::string_view name)
    -> Expected<void, DatacenterRefusal> {
  {
    const platform::Lock lock{mutex_};
    if (over_) return unexpected(DatacenterRefusal::launch_over);
  }
  switch (pick_.submit(name)) {
    case DatacenterLaunchPick::Submit::accepted:
      break;
    case DatacenterLaunchPick::Submit::not_offered:
      return unexpected(DatacenterRefusal::not_offered);
    case DatacenterLaunchPick::Submit::not_waiting:
      return unexpected(DatacenterRefusal::not_waiting);
  }
  listener_.wake();
  return {};
}

auto CloudLaunch::Core::measured_at() const -> DatacenterMeasurement::Time {
  const auto* clock = ps_clock();
  const auto ps_ms = clock != nullptr ? clock->now_ms() : std::nullopt;
  return DatacenterMeasurement::Time{
      std::chrono::milliseconds{ps_ms.value_or(0)}};
}

auto CloudLaunch::Core::ps_clock() const noexcept -> psn::ServerClock* {
  if (doors_.server_clock != nullptr) return doors_.server_clock;
  return request_.user != nullptr
             ? &users::ServerTime::of(*request_.user).server_clock()
             : nullptr;
}

void CloudLaunch::Core::on_progress(
    const LaunchProgress& progress,
    net::protocol::remote_play::FailureStage stage, std::string_view text) {
  const auto now_us = doors_.clock.now_microseconds();
  const auto timed = progress.after(
      std::chrono::microseconds{static_cast<std::chrono::microseconds::rep>(
          now_us > began_us_ ? now_us - began_us_ : 0)});
  {
    const platform::Lock lock{mutex_};
    progress_ = timed;
  }
  listener_.progressed(timed, stage, text);
}

auto CloudLaunch::Core::datacenter_choice_wait() const noexcept
    -> std::optional<std::chrono::milliseconds> {
  return request_.datacenter.wait();
}

void CloudLaunch::Core::on_datacenters(const std::vector<Ping>& picker_rows,
                                       const std::vector<Ping>& measured_rows) {
  std::optional<DatacenterOffer> offered;
  {
    const platform::Lock lock{mutex_};
    measured_rows_ = measured_rows;
    if (!request_.datacenter.name() || request_.datacenter.asks_at_launch())
      measurements_ = Datacenters::measured(measured_rows, measured_at());
    if (request_.datacenter.asks_at_launch()) {
      offer_ = Datacenters::offer_from(picker_rows, request_.datacenter.wait());
      offered = offer_;
    }
  }
  listener_.datacenters_measured(picker_rows);
  if (offered) listener_.datacenters_offered(*offered);
}

void CloudLaunch::Core::on_datacenter_selected(const Ping& selected,
                                               bool forced) {
  std::optional<DatacenterChoice> choice;
  int reported_rtt_ms = selected.rtt_ms;
  std::vector<DatacenterMeasurement> fresh;
  {
    const platform::Lock lock{mutex_};
    if (forced) reported_rtt_ms = picked_rtt_ms_.value_or(0);
    const auto measured =
        reported_rtt_ms > 0
            ? std::optional{std::chrono::milliseconds{reported_rtt_ms}}
            : std::nullopt;
    if (auto name = DatacenterName::parse(selected.name)) {
      using How = DatacenterChoice::How;
      switch (how_.value_or(forced ? How::preferred : How::automatic)) {
        case How::automatic:
          choice = DatacenterChoice::automatic(std::move(*name), measured);
          break;
        case How::preferred:
          choice = DatacenterChoice::preferred(
              std::move(*name), doors_.datacenters != nullptr
                                    ? doors_.datacenters->known(service_)
                                    : KnownDatacenters{});
          break;
        case How::picked_at_launch:
          choice =
              DatacenterChoice::picked_at_launch(std::move(*name), measured);
          break;
        case How::lowest_at_launch:
          choice =
              DatacenterChoice::lowest_at_launch(std::move(*name), measured);
          break;
        case How::lowest_after_waiting:
          choice = DatacenterChoice::lowest_after_waiting(std::move(*name),
                                                          measured);
          break;
      }
    }
    chosen_ = choice;
    fresh = measurements_;
  }
  if (choice && doors_.datacenters != nullptr)
    static_cast<void>(doors_.datacenters->record(service_, fresh, *choice));
  listener_.datacenter_chosen(choice, selected, forced, reported_rtt_ms);
}

auto CloudLaunch::Core::cached_bandwidth(const Ping& selected)
    -> std::optional<CachedBandwidth> {
  if (doors_.datacenters == nullptr || selected.rtt_ms <= 0)
    return std::nullopt;
  return doors_.datacenters->cached_bandwidth(
      service_, selected.name, std::chrono::milliseconds{selected.rtt_ms},
      measured_at());
}

void CloudLaunch::Core::on_bandwidth_measured(const Ping& selected, int kbps) {
  if (doors_.datacenters == nullptr || selected.rtt_ms <= 0) return;
  static_cast<void>(doors_.datacenters->note_bandwidth(
      service_, selected.name, kbps, measured_at(),
      std::chrono::milliseconds{selected.rtt_ms}));
}

auto CloudLaunch::Core::selects_datacenter_at_launch() const noexcept -> bool {
  return request_.datacenter.asks_at_launch();
}

void CloudLaunch::Core::offer_datacenters(
    const std::vector<std::string>& names) {
  {
    const platform::Lock lock{mutex_};
    picked_rtt_ms_.reset();
    how_.reset();
  }
  pick_.open(names);
}

auto CloudLaunch::Core::await_datacenter_choice(const platform::StopToken& stop)
    -> LaunchChoice {
  using Kind = LaunchChoice::Kind;
  auto outcome = pick_.await(
      request_.datacenter.wait(), stop,
      [this] { return doors_.clock.now_microseconds(); },
      [this](std::chrono::microseconds remaining) {
        listener_.idle(remaining);
      });
  if (outcome) return resolved(std::move(outcome.value()));
  if (outcome.error() == LaunchPickMiss::cancelled) {
    listener_.note("cloud datacenter choice cancelled");
    return {Kind::cancelled, {}};
  }
  {
    const platform::Lock lock{mutex_};
    how_ = DatacenterChoice::How::lowest_after_waiting;
  }
  listener_.note("cloud datacenter choice timed out; choosing automatically");
  return {Kind::timed_out, {}};
}

auto CloudLaunch::Core::resolved(std::string name) -> LaunchChoice {
  using Kind = LaunchChoice::Kind;
  if (name == automatic_choice) {
    {
      const platform::Lock lock{mutex_};
      how_ = DatacenterChoice::How::lowest_at_launch;
    }
    listener_.note("cloud datacenter choice picked: automatic");
    return {Kind::automatic, {}};
  }
  {
    const platform::Lock lock{mutex_};
    how_ = DatacenterChoice::How::picked_at_launch;
    for (const auto& row : measured_rows_)
      if (row.name == name && row.measured) picked_rtt_ms_ = row.rtt_ms;
  }
  listener_.note("cloud datacenter choice picked: " + name);
  return {Kind::named, std::move(name)};
}

auto CloudLaunch::Core::forced_datacenter() const -> std::string_view {
  const auto& name = request_.datacenter.name();
  return name ? std::string_view{name->text()} : std::string_view{};
}

auto CloudLaunch::Core::prior_rows() const -> std::vector<Ping> {
  if (!request_.prior.empty() || doors_.datacenters == nullptr)
    return request_.prior;
  return Datacenters::prior_rows(doors_.datacenters->known(service_));
}

auto CloudLaunch::Core::launch_spec(std::string_view entitlement,
                                    std::string_view language) const
    -> Expected<psn::gk::LaunchSpec, Precondition> {
  if (request_.user == nullptr || !request_.user->has_psn_account())
    return unexpected(Precondition::invalid_npsso);
  if (request_.bitrate_kbps <= 0)
    return unexpected(Precondition::invalid_bitrate);
  const bool ps5 = service_ == catalog::ServiceGeneration::ps5_cloud;
  const auto protocol_version = net::protocol::takion::select_takion_version(
      ps5 ? net::protocol::takion::takion_version_cloud_stream
          : net::protocol::takion::takion_version_legacy_cloud_stream,
      request_.takion_version_override,
      ps5 ? net::protocol::takion::SessionScope::cloud_stream
          : net::protocol::takion::SessionScope::legacy_cloud_stream);
  const psn::gk::Language spoken{language};
  const psn::gk::TimeZone time_zone{request_.time_zone, doors_.utc_offset};
  auto spec = ps5 ? psn::gk::LaunchSpec::cloud_stream(
                        entitlement, spoken, time_zone, request_.resolution,
                        protocol_version, request_.requested_audio_channels,
                        request_.hdr, request_.accessibility,
                        request_.high_quality, request_.max_audio_channels)
                  : psn::gk::LaunchSpec::legacy_cloud_stream(
                        entitlement, spoken, time_zone, request_.resolution,
                        protocol_version, request_.accessibility);
  if (!spec) return unexpected(Precondition::invalid_spec);
  return std::move(spec).value();
}

auto CloudLaunch::Core::precondition() const -> std::optional<Precondition> {
  if (request_.time_zone.find('\0') != std::string::npos)
    return Precondition::invalid_spec;
  const auto spec = launch_spec(request_.product.text(), {});
  if (!spec) return spec.error();
  return std::nullopt;
}

auto CloudLaunch::Core::check_device() -> Expected<void, psn::gk::Error> {
  return check_device(doors_.http);
}

auto CloudLaunch::Core::check_device(platform::HttpProvider& http)
    -> Expected<void, psn::gk::Error> {
  using Reason = psn::gk::Error::Reason;
  using Stage = psn::gk::Error::Stage;
  if (request_.stop.stop_requested())
    return unexpected(psn::gk::Error{Stage::identity, Reason::cancelled});
  if (request_.user == nullptr)
    return unexpected(psn::gk::Error{Stage::identity, Reason::invalid_npsso});
  if (const auto checking =
          LaunchProgress::at(service_, LaunchActivity::checking_device)) {
    const platform::Lock lock{mutex_};
    progress_ = *checking;
  }
  psn::ServerClockHttp timed{http, *ps_clock()};
  net::http::Client client{timed, doors_.clock, request_.stop};
  users::PlayCalls user{*request_.user};
  auto device = device_checked(user, client, streaming_service(service_));
  if (!device) return unexpected(device.error());
  return {};
}

auto CloudLaunch::Core::device_checked(users::PlayCalls& user,
                                       net::http::Client& http,
                                       psn::StreamingService service)
    -> Expected<users::DeviceId, psn::gk::Error> {
  using Stage = psn::gk::Error::Stage;
  auto device = user.cloud_device();
  if (!device)
    return unexpected(
        ServerReservation::refused(Stage::identity, device.error()));
  const auto checked = user.cloud_device_check(http, service);
  if (!checked)
    return unexpected(
        ServerReservation::refused(Stage::identity, checked.error()));
  return std::move(device.value());
}

auto CloudLaunch::Core::clear(platform::HttpProvider&)
    -> Expected<catalog::ClearedLaunch, LaunchFailure> {
  if (const auto* cleared = already_cleared()) return *cleared;
  LaunchReport{*this, service_}.say(
      LaunchActivity::clearing_with_catalog,
      net::protocol::remote_play::FailureStage::cloud_launch_check,
      "Checking PlayStation Plus Premium and title streaming");
  const catalog::LaunchCheckObserver observer{
      [this](catalog::LaunchCheck check) { report(check); }};
  const auto verdict = doors_.clearance.clear(*request_.user, request_.product,
                                              request_.stop, observer);
  if (const auto& refusal = verdict.refusal())
    return unexpected(LaunchFailure::not_cleared(*refusal));
  const auto& cleared = verdict.cleared_launch();
  if (!cleared)
    return unexpected(LaunchFailure::not_cleared(
        catalog::LaunchRefusal::check_unavailable(catalog::LaunchCheckFailure{
            catalog::LaunchCheckReason::malformed})));
  return *cleared;
}

auto CloudLaunch::Core::already_cleared() const
    -> const catalog::ClearedLaunch* {
  const auto& cleared = request_.cleared;
  if (!cleared) return nullptr;
  const bool same_product =
      cleared->product == request_.product ||
      (cleared->held_as && request_.product == cleared->held_as->text());
  return same_product ? &*cleared : nullptr;
}

void CloudLaunch::Core::report(catalog::LaunchCheck check) {
  using Check = catalog::LaunchCheck;
  const LaunchReport progress{*this, service_};
  const auto stage =
      net::protocol::remote_play::FailureStage::cloud_launch_check;
  switch (check) {
    case Check::loading_library:
      progress.say(LaunchActivity::loading_library, stage,
                   "Loading your games");
      return;
    case Check::membership:
      progress.say(LaunchActivity::checking_membership, stage,
                   "Checking PlayStation Plus membership");
      return;
    case Check::cloud_streaming:
      progress.say(LaunchActivity::checking_cloud_streaming, stage,
                   "Checking cloud streaming access");
      return;
    case Check::ownership:
      progress.say(LaunchActivity::checking_ownership, stage,
                   "Checking that you own the game");
      return;
    case Check::entitlement:
      progress.say(LaunchActivity::checking_entitlement, stage,
                   "Checking your copy of the game");
      return;
    case Check::cloud_game:
      progress.say(LaunchActivity::resolving_cloud_game, stage,
                   "Finding the cloud version of the game");
      return;
    case Check::exclusions:
      progress.say(LaunchActivity::checking_exclusions, stage,
                   "Checking the game is not restricted");
      return;
    case Check::release:
      progress.say(LaunchActivity::checking_release, stage,
                   "Checking the release date");
      return;
  }
}

auto CloudLaunch::Core::target(const catalog::ClearedLaunch& cleared,
                               unsigned round)
    -> Expected<Target, LaunchFailure> {
  if (auto* ps5 = std::get_if<Ps5CloudLaunch>(&sequence_))
    return Target{ps5->entitlement(), ps5->platform()};
  auto& old_service = std::get<OldServiceLaunch>(sequence_);
  auto prepared =
      diagnostics::observe(doors_.trace, diagnostics::Operation::cloud_title,
                           [&] { return old_service.prepare(cleared, round); });
  if (!prepared) return unexpected(LaunchFailure::title(prepared.error()));
  return Target{prepared.value().entitlement(), prepared.value().platform()};
}

auto CloudLaunch::Core::repeats_after_refusal(
    const catalog::ClearedLaunch& cleared, unsigned round) const -> bool {
  if (const auto* ps5 = std::get_if<Ps5CloudLaunch>(&sequence_))
    return ps5->retries_reservation_after_refusal(round);
  return OldServiceLaunch::repeats_title_steps_after_refusal(cleared, round);
}

auto CloudLaunch::Core::reserve(platform::HttpProvider& http,
                                psn::gk::LaunchSpec spec,
                                psn::StreamingPlatform platform,
                                const std::vector<Ping>& prior)
    -> Expected<ServerSlot, psn::gk::Error> {
  const auto* trace = doors_.trace;
  net::http::Client reservation_http{http, doors_.clock, request_.stop,
                                     trace ? trace->sink() : nullptr};
  psn::gk::Client client{reservation_http};
  if (request_.user) {
    auto settled = ServerReservation::settle(
        client, {doors_.waiter, trace, *this}, spec.service(),
        users::CloudRelaunch::of(*request_.user));
    if (!settled) return unexpected(settled.error());
  }
  net::http::Client account_http{http, doors_.clock, request_.stop};
  users::PlayCalls user{*request_.user};
  auto locked = ServerReservation::open_locked(
      client, {doors_.waiter, trace, *this}, spec, platform, user, account_http);
  if (!locked) return unexpected(locked.error());
  auto& reservation = locked.value();
  const auto measure_up_to =
      request_.measure_bandwidth
          ? std::optional<std::uint32_t>{ServerReservation::bandwidth_test_kbps(
                request_.resolution)}
          : std::nullopt;
  return reservation.provision({doors_.network, doors_.clock, doors_.threading,
                                doors_.random, doors_.waiter},
                               forced_datacenter(), prior,
                               request_.bitrate_kbps, measure_up_to);
}

void CloudLaunch::Core::note_trial_refusal(const LaunchFailure& failure) {
  if (request_.stop.stop_requested()) return;
  std::string line =
      std::format("[TRIAL] launch refused after the launch checks for {}: {}",
                  request_.product.text(), failure.describe());
  if (const auto* reservation =
          failure.as<LaunchFailure::ReservationFailed>()) {
    const auto& error = reservation->error();
    line += std::format("; stage {} reason {} detail {} HTTP {}",
                        magic_enum::enum_name(error.stage()),
                        magic_enum::enum_name(error.reason()), error.detail(),
                        error.http_status());
    const auto remembered = reservation->ps_refusal();
    line += remembered ? std::format("; remembered as {}",
                                     magic_enum::enum_name(*remembered))
                       : std::string{"; not remembered"};
    if (!error.rejection().empty())
      line += std::format("; PS replied: {}", error.rejection());
  } else {
    line += std::format("; HTTP {}", failure.http_status());
  }
  listener_.note(line);
}

void CloudLaunch::Core::remember(const LaunchFailure& failure) {
  const platform::Lock lock{mutex_};
  refusal_ = failure.refused();
  over_ = true;
}

auto CloudLaunch::Core::attempt(platform::HttpProvider& http)
    -> Expected<ServerSlot, LaunchFailure> {
  using Reason = psn::gk::Error::Reason;
  const auto* trace = doors_.trace;
  if (const auto failed = precondition())
    return unexpected(LaunchFailure::precondition(*failed));

  auto identified = diagnostics::observe(
      trace, diagnostics::Operation::cloud_identity,
      [&]() -> Expected<void, TracedIdentity> {
        auto checked = check_device(http);
        if (!checked)
          return unexpected(
              TracedIdentity{static_cast<TraceCode>(checked.error().detail()),
                             checked.error()});
        return {};
      });
  if (!identified)
    return unexpected(LaunchFailure::reservation(
        identified.error().error, forced_datacenter(),
        request_.stop.stop_requested()));

  auto cleared = clear(http);
  if (!cleared) return unexpected(std::move(cleared.error()));
  {
    const platform::Lock lock{mutex_};
    service_ = cleared.value().service;
  }
  trial_ = cleared.value().trial;
  if (service_ == catalog::ServiceGeneration::old_service) {
    title_http_.emplace(http, doors_.clock, request_.stop);
    sequence_.emplace<OldServiceLaunch>(
        *title_http_, *request_.user, LaunchReport{*this, service_},
        OldServiceLaunch::Options{request_.skip_account_attributes_check});
  } else {
    sequence_.emplace<Ps5CloudLaunch>(cleared.value());
  }

  const auto prior = prior_rows();
  for (unsigned round = 0; round < 2; ++round) {
    if (round && trace)
      trace->decision(diagnostics::Trace::Decision::owned_entitlement, round);
    auto aimed = target(cleared.value(), round);
    if (!aimed) return unexpected(std::move(aimed.error()));
    if (trace && trace->sink() != nullptr &&
        service_ == catalog::ServiceGeneration::old_service)
      trace->sink()->write(
          {platform::LogSeverity::info,
           std::format("legacy_cloud_stream: start {} of {} sends entitlement "
                       "{} from {}",
                       round + 1, cleared.value().product.text(),
                       aimed.value().entitlement,
                       round == 0 && cleared.value().held_as
                           ? "the owned record"
                           : "the LegacyCloudStream store")});
    auto spec = launch_spec(aimed.value().entitlement,
                            cleared.value().store.language());
    if (!spec) return unexpected(LaunchFailure::precondition(spec.error()));
    auto reserved =
        reserve(http, std::move(spec.value()), aimed.value().platform, prior);
    if (reserved) {
      reserved.value().service = streaming_service(service_);
      reserved.value().platform = aimed.value().platform;
      reserved.value().entitlement_id = std::move(aimed.value().entitlement);
      return std::move(reserved.value());
    }
    const bool stopped = request_.stop.stop_requested();
    const bool no_game = reserved.error().reason() == Reason::entitlement_retry;
    if (no_game && !stopped && round == 0 &&
        repeats_after_refusal(cleared.value(), round)) {
      LaunchProgress::Counts again;
      again.attempt = round + 2;
      again.wait = progress::Wait{progress::WaitReason::title_not_ready,
                                  std::chrono::milliseconds{0}};
      LaunchReport{*this, service_}.say(
          LaunchActivity::reserving_server,
          net::protocol::remote_play::FailureStage::cloud_start,
          "Starting Session again because the title was not ready", again);
      continue;
    }
    if (no_game && !stopped) {
      const auto kind = catalog::StreamRefusal::Kind::no_game_for_entitlement;
      doors_.clearance.remember_refusal(*request_.user, request_.product, kind);
      return unexpected(LaunchFailure::refused_by_service(
          reserved.error(), forced_datacenter(), kind));
    }
    return unexpected(LaunchFailure::reservation(reserved.error(),
                                                 forced_datacenter(), stopped));
  }
  return unexpected(LaunchFailure::precondition(Precondition::invalid_spec));
}

auto CloudLaunch::Core::run() -> Expected<ServerSlot, LaunchFailure> {
  began_us_ = doors_.clock.now_microseconds();
  auto slot = attempt(doors_.http);
  if (!slot) {
    if (trial_) note_trial_refusal(slot.error());
    remember(slot.error());
  } else {
    const platform::Lock lock{mutex_};
    over_ = true;
  }
  return slot;
}

}  // namespace libchiaking::play
