#include "src/core/catalog/usecases/cloud_stream/clearance.hpp"

#include "src/core/catalog/adapters/ps/cloud_game_resolver.hpp"
#include "src/core/catalog/adapters/ps/ownership_records.hpp"
#include "src/core/catalog/adapters/ps/ps5_library.hpp"
#include "src/core/catalog/adapters/ps/store_app_title.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>
#include <magic_enum/magic_enum.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace libchiaking::catalog::usecases::cloud_stream {

Clearance::Clearance(platform::HttpProvider& http,
                     const platform::ClockProvider& clock,
                     const platform::ClockProvider& wall_clock,
                     platform::DiagnosticsProvider* log, HeldAnswers& held,
                     ListGames& list, LaunchChecks& checks) noexcept
    : http_(http),
      clock_(clock),
      wall_(wall_clock),
      log_(log),
      held_(held),
      list_(list),
      checks_(checks),
      exclusions_(http, clock, wall_clock, log, held) {}

auto Clearance::can_launch(users::PsUser& user, const View& seen,
                           const ProductId& product) -> LaunchVerdict {
  const auto* game = seen.games->find(product);
  if (game == nullptr)
    return LaunchVerdict::refused(LaunchRefusal::not_listed());
  const auto hidden = game->visibility().hide_reason();
  if (hidden && *hidden != HideReason::tier_does_not_include)
    return LaunchVerdict::refused(LaunchRefusal::hidden(*hidden));
  auto streaming = LaunchChecks::held_streaming_refusal(user);
  if (hidden)
    return streaming ? std::move(*streaming)
                     : LaunchVerdict::refused(LaunchRefusal::hidden(*hidden));
  const auto route = Route::of(*game, product);
  if (route.is_a_choice()) return LaunchVerdict::refused(*route.refusal());
  if (game->ownership() && game->ownership()->trial_ended())
    return LaunchVerdict::refused(LaunchRefusal::purchase_needed());
  if (auto remembered = remembered_refusal(*game))
    return std::move(*remembered);
  if (streaming) return std::move(*streaming);
  const auto& owned = route.owned_entitlement();
  if (!owned) return LaunchVerdict::refused(*route.refusal());
  const auto record = held_record(user, *owned);
  const auto subject = exclusion_subject(record, *game);
  if (auto excluded =
          exclusions_.held(subject.title, subject.product).refusal())
    return LaunchVerdict::refused(std::move(*excluded));
  if (auto unreleased = checks_.held_release_refusal(
          *game, record ? record->release() : game->release()))
    return LaunchVerdict::refused(std::move(*unreleased));
  return cleared(*game, seen, user, *owned);
}

auto Clearance::clear(users::PsUser& user, View seen, const ProductId& product,
                      std::optional<users::CloudStreaming> streaming,
                      const Stop& stop, CheckProgress progress)
    -> LaunchVerdict {
  Attempt attempt{.seen = std::move(seen),
                  .asked = product,
                  .streaming = std::move(streaming),
                  .progress = progress};
  attempt.game = attempt.seen.games->find(product);
  auto verdict = run(user, stop, attempt);
  const auto* refusal = verdict.refusal();
  if (refusal != nullptr && is_trial(attempt.game))
    note_trial_refusal(*refusal, attempt);
  return verdict;
}

auto Clearance::run(users::PsUser& user, const Stop& stop, Attempt& attempt)
    -> LaunchVerdict {
  for (;;) {
    if (auto ended = judge(user, attempt, ask(user, stop, attempt)))
      return std::move(*ended);
  }
}

auto Clearance::ask(users::PsUser& user, const Stop& stop, Attempt& attempt)
    -> CheckAnswer {
  switch (attempt.lifecycle.stage()) {
    case Stage::standing:
    case Stage::access_route:
      return std::monostate{};
    case Stage::cloud_streaming:
      if (attempt.streaming) return std::monostate{};
      attempt.progress.started(LaunchCheck::cloud_streaming);
      return checks_.cloud_streaming_at_launch(user, stop);
    case Stage::ownership: {
      attempt.progress.started(LaunchCheck::ownership);
      const auto answer = ownership(user, attempt.seen, attempt.asked, stop);
      if (!answer)
        return OwnershipCheck{OwnershipCheck::Failed{answer.error()}};
      if (!answer.value().is_owned())
        return OwnershipCheck{OwnershipCheck::NotOwned{}};
      return OwnershipCheck{OwnershipCheck::Owned{
          checks_.held_or_loaded(user, stop, attempt.progress)}};
    }
    case Stage::entitlement: {
      attempt.progress.started(LaunchCheck::entitlement);
      net::http::Client client{http_, clock_, stop, log_};
      return ps::Ps5Library{user, client, wall_}.one(
          *attempt.route->owned_entitlement());
    }
    case Stage::selected_title: {
      attempt.progress.started(LaunchCheck::cloud_game);
      net::http::Client client{http_, clock_, stop, log_};
      return ps::CloudGameResolver{user, client}.for_product(
          attempt.game->product_id(), attempt.streaming->plans());
    }
    case Stage::exclusions: {
      attempt.progress.started(LaunchCheck::exclusions);
      const auto subject = exclusion_subject(attempt.record, *attempt.game);
      return exclusions_.of(subject.title, subject.product, stop);
    }
    case Stage::release:
      attempt.progress.started(LaunchCheck::release);
      return checks_.release_refusal(*attempt.game, attempt.record->release(),
                                     stop);
  }
  return std::monostate{};
}

auto Clearance::judge(users::PsUser& user, Attempt& attempt, CheckAnswer answer)
    -> Ended {
  switch (attempt.lifecycle.stage()) {
    case Stage::standing:
      return judge_standing(attempt);
    case Stage::cloud_streaming:
      if (auto* read = std::get_if<users::CloudStreaming>(&answer))
        attempt.streaming = std::move(*read);
      return judge_cloud_streaming(user, attempt);
    case Stage::access_route:
      return take_the_access_route(attempt);
    case Stage::ownership:
      return judge_ownership(attempt,
                             std::move(std::get<OwnershipCheck>(answer)));
    case Stage::entitlement:
      return judge_entitlement(
          user, attempt,
          std::move(
              std::get<Expected<std::optional<Ps5Entitlement>, Unanswered>>(
                  answer)));
    case Stage::selected_title:
      return judge_selected_title(
          attempt, std::get<catalog::cloud_stream::CloudGameListing>(answer));
    case Stage::exclusions:
      return judge_exclusions(attempt,
                              std::get<CheckExclusion::Verdict>(answer));
    case Stage::release:
      return judge_release(
          user, attempt,
          std::move(std::get<std::optional<LaunchRefusal>>(answer)));
  }
  return std::nullopt;
}

auto Clearance::judge_standing(Attempt& attempt) -> Ended {
  if (attempt.game == nullptr)
    return LaunchVerdict::refused(LaunchRefusal::not_listed());
  if (const auto hidden = attempt.game->visibility().hide_reason())
    return LaunchVerdict::refused(LaunchRefusal::hidden(*hidden));
  if (const auto route = Route::of(*attempt.game, attempt.asked);
      route.is_a_choice())
    return LaunchVerdict::refused(*route.refusal());
  if (auto remembered = remembered_refusal(*attempt.game)) return remembered;
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_cloud_streaming(users::PsUser& user, Attempt& attempt)
    -> Ended {
  if (auto refused = LaunchChecks::refused_streaming(user, *attempt.streaming))
    return refused;
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::take_the_access_route(Attempt& attempt) -> Ended {
  attempt.route = Route::of(*attempt.game, attempt.asked);
  if (attempt.route->needs_ownership_check()) {
    attempt.lifecycle.need_ownership();
    return std::nullopt;
  }
  if (!attempt.route->owned_entitlement())
    return LaunchVerdict::refused(*attempt.route->refusal());
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_ownership(Attempt& attempt, OwnershipCheck check)
    -> Ended {
  if (const auto* failed = std::get_if<OwnershipCheck::Failed>(&check.outcome))
    return LaunchChecks::unavailable(failed->cause);
  if (std::holds_alternative<OwnershipCheck::NotOwned>(check.outcome))
    return LaunchVerdict::refused(attempt.route->refusal_when_not_owned());
  auto& again = std::get<OwnershipCheck::Owned>(check.outcome).reloaded;
  if (!again) return LaunchChecks::unavailable(again.error());
  attempt.seen = std::move(again).value();
  attempt.game = attempt.seen.games->find(attempt.asked);
  if (attempt.game == nullptr)
    return LaunchVerdict::refused(LaunchRefusal::not_listed());
  attempt.route = Route::of(*attempt.game, attempt.asked);
  if (!attempt.route->owned_entitlement())
    return LaunchVerdict::refused(*attempt.route->refusal());
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_entitlement(
    users::PsUser& user, Attempt& attempt,
    Expected<std::optional<Ps5Entitlement>, Unanswered> reread) -> Ended {
  if (!reread) return LaunchChecks::unavailable(reread.error());
  attempt.record = std::move(reread.value());
  if (auto refused = refusal_of(attempt.record)) {
    if (is_trial(attempt.game) && ends_the_trial(*refused)) {
      list_.remember_refusal(user, attempt.game->product_id(),
                             StreamRefusal::Kind::trial_ended);
      attempt.remembered_as_ended = true;
    }
    return LaunchVerdict::refused(std::move(*refused));
  }
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_selected_title(
    Attempt& attempt, const catalog::cloud_stream::CloudGameListing& listing)
    -> Ended {
  if (auto cause = listing.unanswered())
    return LaunchChecks::unavailable(std::move(*cause));
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_exclusions(Attempt& attempt,
                                 const CheckExclusion::Verdict& verdict)
    -> Ended {
  if (auto excluded = verdict.refusal())
    return LaunchVerdict::refused(std::move(*excluded));
  attempt.lifecycle.pass();
  return std::nullopt;
}

auto Clearance::judge_release(const users::PsUser& user, const Attempt& attempt,
                              std::optional<LaunchRefusal> unreleased)
    -> LaunchVerdict {
  if (unreleased) return LaunchVerdict::refused(std::move(*unreleased));
  return cleared(*attempt.game, attempt.seen, user, attempt.record->id());
}

auto Clearance::remembered_refusal(const MyGame& game) -> Ended {
  const auto& remembered = game.remembered_refusal();
  if (!remembered) return std::nullopt;
  if (remembered->ends_the_trial())
    return LaunchVerdict::refused(LaunchRefusal::purchase_needed());
  return LaunchVerdict::refused(
      LaunchRefusal::refused_by_service(remembered->refused_at()));
}

auto Clearance::is_trial(const MyGame* game) noexcept -> bool {
  return game != nullptr && game->ownership() &&
         game->ownership()->held_as() == HeldAs::trial_access;
}

auto Clearance::ends_the_trial(const LaunchRefusal& refusal) noexcept -> bool {
  using Kind = LaunchRefusal::Kind;
  switch (refusal.kind()) {
    case Kind::purchase_needed:
    case Kind::title_taken_down:
      return true;
    default:
      return false;
  }
}

void Clearance::note_trial_refusal(const LaunchRefusal& refusal,
                                   const Attempt& attempt) const {
  const auto cause = refusal.cause();
  if (cause && cause->is_cancelled()) return;
  const auto flag = [](const std::optional<bool>& value) -> std::string_view {
    if (!value) return "absent";
    return *value ? "true" : "false";
  };
  std::string line = std::format(
      "[TRIAL] launch refused by the launch checks for {} "
      "(entitlement {}) at {}: {}",
      attempt.game->product_id().text(), attempt.game->ownership()->id().text(),
      magic_enum::enum_name(attempt.lifecycle.stage()),
      magic_enum::enum_name(refusal.kind()));
  if (cause)
    line += std::format("; unanswered {} HTTP {}",
                        magic_enum::enum_name(cause->reason()),
                        cause->http_status());
  if (attempt.lifecycle.stage() == Stage::entitlement) {
    if (const auto& record = attempt.record) {
      const auto& facts = record->facts();
      line += std::format(
          "; re-read isActive={} streamingSupported={} "
          "isPlayTimeFullyConsumed={} featureType={}",
          flag(facts.active), flag(facts.streaming_supported),
          flag(facts.play_time_used_up),
          facts.feature_type ? std::to_string(*facts.feature_type) : "absent");
    } else {
      line += "; re-read found no record";
    }
  }
  line += attempt.remembered_as_ended ? "; remembered as an ended trial"
                                      : "; not remembered as ended";
  if (log_ != nullptr) log_->write({platform::LogSeverity::info, line});
}

auto Clearance::cleared(const MyGame& game, const View& seen,
                        const users::PsUser& user, const OwnershipId& owned)
    -> LaunchVerdict {
  auto launch = LaunchChecks::clearance(game, seen, user);
  launch.identifier = LaunchIdentifier::of(owned);
  launch.held_as = owned;
  launch.trial = is_trial(&game);
  return LaunchVerdict::cleared(std::move(launch));
}

auto Clearance::exclusion_subject(const std::optional<Ps5Entitlement>& record,
                                  const MyGame& game) -> ExclusionSubject {
  if (!record) return ExclusionSubject{std::nullopt, game.product_id()};
  const auto& facts = record->facts();
  return ExclusionSubject{facts.title,
                          facts.product.value_or(game.product_id())};
}

auto Clearance::held_record(const users::PsUser& user, const OwnershipId& id)
    -> std::optional<Ps5Entitlement> {
  const auto owned = held_.held_by(user.account_id());
  if (!owned || !owned->ps5) return std::nullopt;
  const auto* record = owned->ps5->find(id);
  return record != nullptr ? std::optional{*record} : std::nullopt;
}

auto Clearance::refusal_of(const std::optional<Ps5Entitlement>& reread)
    -> std::optional<LaunchRefusal> {
  if (!reread || !reread->facts().product)
    return LaunchRefusal::invalid_title();
  if (!reread->why_not_a_game()) return std::nullopt;
  const auto& facts = reread->facts();
  const bool active = facts.active == true;
  if (facts.streaming_supported != true)
    return active ? LaunchRefusal::streaming_unavailable()
                  : LaunchRefusal::title_taken_down();
  if (!active || facts.play_time_used_up == true)
    return LaunchRefusal::purchase_needed();
  return LaunchRefusal::unexpected_entitlement_state();
}

auto Clearance::ownership(users::PsUser& user, const View& seen,
                          const ProductId& asked, const Stop& stop)
    -> Expected<OwnershipAnswer, Unanswered> {
  const auto* game = seen.games->find(asked);
  if (game == nullptr)
    return unexpected(Unanswered{Unanswered::Reason::not_listed});
  const auto account = user.account_id();
  const auto ids = Route::of(*game, asked).rechecked_by();
  const auto list = seen.list ? seen.list.value() : nullptr;
  const bool library_known =
      seen.games->ownership_is_known_on(ServiceGeneration::ps5_cloud);
  return held_.in_flight().rechecks.answer(
      HeldAnswers::Question::by(account, asked).text(), stop,
      [&]() -> Expected<OwnershipAnswer, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        std::vector<OwnershipId> on_ps5;
        std::vector<OwnershipId> on_legacy_cloud_stream;
        for (const auto& id : ids)
          (ps::Ps5Library::asks_about(id) ? on_ps5 : on_legacy_cloud_stream)
              .push_back(id);
        const auto ps5 = ps::Ps5Library{user, client, wall_}.one_of(on_ps5);
        if (!ps5) return unexpected(ps5.error());
        HeldAnswers::Owned rechecked;
        bool refetched = false;
        if (ps5.value()) {
          if (const auto why = ps5.value()->why_not_a_game())
            return OwnershipAnswer::not_a_game(*why);
          if (!ps5.value()->as_owned_title())
            return OwnershipAnswer::not_owned();
          rechecked = held_.kept_with(account, *ps5.value());
        } else if (auto legacy_cloud_stream = legacy_cloud_stream_record(
                       user, client, on_legacy_cloud_stream);
                   !legacy_cloud_stream) {
          return unexpected(legacy_cloud_stream.error());
        } else if (legacy_cloud_stream.value()) {
          if (const auto why = legacy_cloud_stream.value()->why_not_a_game())
            return OwnershipAnswer::not_a_game(*why);
          auto title = legacy_cloud_stream.value()->as_owned_title();
          if (!title) return OwnershipAnswer::not_owned();
          rechecked = held_.kept_with(account, std::move(*title));
        } else {
          auto owned = list_.library_afresh(user, stop);
          if (!owned && (library_known || Asker::ends_the_load(owned.error())))
            return unexpected(owned.error());
          if (!owned) return OwnershipAnswer::not_owned();
          rechecked = held_.owned_after(account, std::move(owned).value());
          refetched = true;
        }
        held_.drop_storefront_answer(account, asked);
        const auto games =
            list_.matched(user, list, rechecked, list_.category_held_for(user),
                          list_.for_sale_held_for(user));
        const auto* now_listed = games->find(asked);
        if (now_listed != nullptr &&
            Route::of(*now_listed, asked).owned_entitlement())
          return OwnershipAnswer::owned(*now_listed);
        if (refetched &&
            rechecked.titles->ps5_ownership() != LibraryState::fetched)
          return unexpected(Unanswered{Unanswered::Reason::unavailable});
        return OwnershipAnswer::not_owned();
      });
}

auto Clearance::legacy_cloud_stream_record(users::PsUser& user,
                                           net::http::Client& client,
                                           std::span<const OwnershipId> ids)
    -> Expected<std::optional<OwnedRecord>, Unanswered> {
  if (ids.empty()) return std::optional<OwnedRecord>{};
  auto found = ps::OwnershipRecords{user, client, wall_}.one_of(ids);
  if (!found || !found.value() ||
      found.value()->service() == ServiceGeneration::old_service)
    return found;
  return std::optional<OwnedRecord>{};
}

auto Clearance::storefront(users::PsUser& user, const ProductId& product,
                           const std::optional<ConceptId>& grouped_under,
                           const Stop& stop)
    -> Expected<StorefrontAnswer, Unanswered> {
  const auto account = user.account_id();
  return HeldAnswers::answer(
      held_.in_flight().storefront, HeldAnswers::Question::by(account, product),
      stop, [&] { return held_.fresh_storefront_answer(account, product); },
      [&]() -> Expected<StorefrontAnswer, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        ps::StoreAppTitle store{user, client, wall_};
        auto answer = store.answer_for(product);
        if (!answer && grouped_under &&
            answer.error().reason() == Unanswered::Reason::malformed) {
          client.log(platform::LogSeverity::info,
                     "[STORE] " + product.text() +
                         " has no store listing; asking about its concept");
          answer = store.answer_for(product, *grouped_under);
        }
        if (answer) held_.keep(account, product, answer.value());
        return answer;
      });
}

}  // namespace libchiaking::catalog::usecases::cloud_stream
