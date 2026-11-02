/// @file
/// @brief Declares usecases::cloud_stream::Clearance, the launch checks of a
/// PS5 cloud streaming game and the ownership recheck and storefront questions
/// behind them.
///
/// Cloud streaming access is read fresh; the owned entitlement is re-read
/// and must still be a streamable full game; the selected-title resolver is
/// awaited; the exclusion list is applied; the release is judged against
/// trusted time. Every check blocks and any error refuses. The quick pre-check
/// gives the same answer from held facts. A trial whose re-read entitlement
/// PS reports inactive or used up is remembered as ended, and every refused
/// trial launch is logged with a `[TRIAL]` line.
#pragma once

#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/model/cloud_stream/access_route.hpp"
#include "src/core/catalog/model/cloud_stream/clearance_lifecycle.hpp"
#include "src/core/catalog/model/cloud_stream/cloud_game_listing.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/model/library/ps5_entitlement.hpp"
#include "src/core/catalog/usecases/cloud_stream/check_exclusion.hpp"
#include "src/core/catalog/usecases/launch_checks.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <variant>

namespace libchiaking::catalog::usecases::cloud_stream {

/// @brief Clears PS5 cloud games for launch.
///
/// Borrows the provider, clock, log, held answers, ListGames and shared
/// checks, which must outlive it. Safe to call from several threads;
/// concurrent rechecks and storefront questions for one product share one
/// request.
class Clearance {
 public:
  /// @brief The token that abandons a call.
  using Stop = LaunchChecks::Stop;
  /// @brief A MyGames build with what it was built from.
  using View = LaunchChecks::View;

  /// @brief Sets up the clearance.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps answers. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held The held answers. Must outlive this object.
  /// @param list Lists the user's games. Must outlive this object.
  /// @param checks The shared checks. Must outlive this object.
  Clearance(platform::HttpProvider& http, const platform::ClockProvider& clock,
            const platform::ClockProvider& wall_clock,
            platform::DiagnosticsProvider* log, HeldAnswers& held,
            ListGames& list, LaunchChecks& checks) noexcept;

  /// @brief The quick pre-check from held facts; never asks PS.
  ///
  /// Refuses, in order: not listed; hidden for any reason but the tier; hidden
  /// by the tier (with the held streaming refusal when there is one); a
  /// bundle asked by its own product (LaunchRefusal::choose_a_game); an
  /// owned copy that is an ended trial (LaunchRefusal::purchase_needed); a
  /// remembered PS refusal; a held streaming refusal; an access route
  /// without an owned entitlement; an exclusion by the held list (or no fresh
  /// held list); a release not reached by the held trusted time. The release is
  /// read from the held entitlement record when there is one.
  ///
  /// @param user The user.
  /// @param seen The held games.
  /// @param product The product the launch asks for.
  /// @return The verdict.
  auto can_launch(users::PsUser& user, const View& seen,
                  const ProductId& product) -> LaunchVerdict;
  /// @brief Runs the full PS5 checks stage by stage.
  ///
  /// Blocks for each request a stage needs: cloud streaming access (unless
  /// given), an ownership recheck when ownership is unknown, the singular
  /// entitlement re-read, the selected-title resolver, the exclusion list
  /// refresh when it is not fresh, and a trusted time sync for a gated release.
  ///
  /// @param user The user.
  /// @param seen The games.
  /// @param product The product the launch asks for.
  /// @param streaming Access already reread, or std::nullopt to read it now.
  /// @param stop Abandons the checks.
  /// @param progress Told each check as it starts.
  /// @return Cleared through the owned entitlement; otherwise refused with the
  ///     first stage's refusal, or check unavailable when a request failed.
  ///     A refused trial is logged by note_trial_refusal.
  auto clear(users::PsUser& user, View seen, const ProductId& product,
             std::optional<users::CloudStreaming> streaming, const Stop& stop,
             CheckProgress progress = {}) -> LaunchVerdict;
  /// @brief Rechecks whether the user owns a listed PS5 game.
  ///
  /// Asks the PS5 entitlements the route names through the singular endpoint,
  /// then the LegacyCloudStream ones through the commerce lookup, then rereads
  /// the whole library. A copy found is held, the games are rebuilt, and the
  /// answer is read from the rebuilt game's route. The held storefront answer
  /// is dropped.
  ///
  /// @param user The user.
  /// @param seen The games.
  /// @param asked The product asked about.
  /// @param stop Abandons the recheck.
  /// @return Owned with the rebuilt game; not owned; not a game with the
  ///     reason; Unanswered::Reason::not_listed when the game is not listed;
  ///     Unanswered::Reason::unavailable when a reread library still does not
  ///     know PS5 ownership; the lookup's failure.
  [[nodiscard]] auto ownership(users::PsUser& user, const View& seen,
                               const ProductId& asked, const Stop& stop)
      -> Expected<OwnershipAnswer, Unanswered>;
  /// @brief What the store offers the user for a PS5 product, held or asked.
  ///
  /// The product is looked up first. When the store has no listing for it, as
  /// for the cloud-only products some games are offered under, and the game's
  /// concept is known, the concept is asked about instead.
  ///
  /// @param user The user.
  /// @param product The product.
  /// @param grouped_under The game's concept, when known.
  /// @param stop Abandons the call.
  /// @return The answer; the store's failure.
  [[nodiscard]] auto storefront(users::PsUser& user, const ProductId& product,
                                const std::optional<ConceptId>& grouped_under,
                                const Stop& stop)
      -> Expected<StorefrontAnswer, Unanswered>;

 private:
  /// @brief How the user may reach a game.
  using Route = catalog::cloud_stream::AccessRoute;

  /// @brief The checks a clearance goes through, in order.
  using Stage = catalog::cloud_stream::ClearanceStage;

  /// @brief One run of the checks.
  struct Attempt {
    /// @brief The games, replaced after a recheck.
    View seen;
    /// @brief The product the launch asked for.
    ProductId asked;
    /// @brief The game, pointing into seen; null when not listed.
    const MyGame* game{};
    /// @brief The cloud streaming access read.
    std::optional<users::CloudStreaming> streaming{};
    /// @brief The access route taken.
    std::optional<Route> route{};
    /// @brief The re-read entitlement.
    std::optional<Ps5Entitlement> record{};
    /// @brief Which check runs next.
    catalog::cloud_stream::ClearanceLifecycle lifecycle{};
    /// @brief Told each check as it starts.
    CheckProgress progress{};
    /// @brief Whether the refusal was remembered as an ended trial.
    bool remembered_as_ended{};
  };

  /// @brief What an exclusion is matched against.
  struct ExclusionSubject {
    /// @brief The entitlement's title id, when known.
    std::optional<TitleId> title;
    /// @brief The entitlement's product, else the game's.
    ProductId product;
  };

  /// @brief A stage's outcome: a verdict that ends the run, or std::nullopt to
  /// go on.
  using Ended = std::optional<LaunchVerdict>;

  /// @brief What the ownership recheck answered: no answer, not owned, or
  /// owned with the games read again.
  struct OwnershipCheck {
    /// @brief The recheck could not be answered.
    struct Failed {
      /// @brief Why it went unanswered.
      Unanswered cause;
    };
    /// @brief The recheck found the game not owned.
    struct NotOwned {};
    /// @brief The recheck found the game owned, and the games were read
    /// again.
    struct Owned {
      /// @brief The games read again.
      Expected<View, Unanswered> reloaded;
    };
    /// @brief The recheck's outcome.
    std::variant<Failed, NotOwned, Owned> outcome;
  };

  /// @brief The answer the check of a stage brought back: nothing for the
  /// stages that ask nobody, or the stage's own answer.
  using CheckAnswer =
      std::variant<std::monostate, users::CloudStreaming, OwnershipCheck,
                   Expected<std::optional<Ps5Entitlement>, Unanswered>,
                   catalog::cloud_stream::CloudGameListing,
                   CheckExclusion::Verdict, std::optional<LaunchRefusal>>;

  /// @brief Runs the checks from Stage::standing until one ends the run: the
  /// machine names the check, ask() performs it and judge() feeds the answer
  /// back.
  ///
  /// @param user The user.
  /// @param stop Abandons the checks.
  /// @param attempt The run.
  /// @return The verdict of the stage that ended it.
  auto run(users::PsUser& user, const Stop& stop, Attempt& attempt)
      -> LaunchVerdict;
  /// @brief Performs the check the machine names, asking the services it
  /// needs; decides nothing.
  ///
  /// @param user The user.
  /// @param stop Abandons the requests.
  /// @param attempt The run.
  /// @return The check's answer.
  auto ask(users::PsUser& user, const Stop& stop, Attempt& attempt)
      -> CheckAnswer;
  /// @brief Judges a check's answer at the stage the machine names, moving
  /// the machine on when the check passed.
  ///
  /// @param user The user.
  /// @param attempt The run.
  /// @param answer What ask() brought back for the stage.
  /// @return The verdict that ends the run, or std::nullopt to go on.
  auto judge(users::PsUser& user, Attempt& attempt, CheckAnswer answer)
      -> Ended;
  /// @brief Runs Stage::standing.
  ///
  /// @param attempt The run.
  /// @return Not listed, hidden, LaunchRefusal::choose_a_game for a bundle
  ///     asked by its own product, or the remembered refusal; else
  ///     std::nullopt.
  static auto judge_standing(Attempt& attempt) -> Ended;
  /// @brief The refusal a remembered PS refusal gives.
  ///
  /// @param game The game.
  /// @return LaunchRefusal::purchase_needed for a remembered
  ///     StreamRefusal::Kind::trial_ended, LaunchRefusal::refused_by_service
  ///     for any other, or std::nullopt.
  static auto remembered_refusal(const MyGame& game) -> Ended;
  /// @brief Whether a game is listed through a copy held as trial access.
  ///
  /// @param game The game; may be null.
  /// @return True when the game's owned copy is HeldAs::trial_access.
  static auto is_trial(const MyGame* game) noexcept -> bool;
  /// @brief Whether a refusal of a trial's re-read entitlement means the
  /// trial cannot be played any more.
  ///
  /// Only the refusals PS's record gives for an inactive entitlement or
  /// used-up play time count: LaunchRefusal::purchase_needed and
  /// LaunchRefusal::title_taken_down. A missing record, an unexpected state,
  /// streaming switched off for an active copy, and every check that could
  /// not be answered do not.
  ///
  /// @param refusal The refusal.
  /// @return True for those two kinds.
  static auto ends_the_trial(const LaunchRefusal& refusal) noexcept -> bool;
  /// @brief Logs, at info level, a refused launch of a trial with the stage,
  /// the refusal, the unanswered check's reason and HTTP status, the
  /// re-read entitlement's flags and whether it was remembered as ended.
  ///
  /// Nothing is logged for a cancelled check.
  ///
  /// @param refusal The refusal.
  /// @param attempt The run; its game must be a trial.
  void note_trial_refusal(const LaunchRefusal& refusal,
                          const Attempt& attempt) const;
  /// @brief Judges Stage::cloud_streaming.
  ///
  /// @param user The user.
  /// @param attempt The run; its streaming access must be known.
  /// @return The streaming refusal, or std::nullopt.
  static auto judge_cloud_streaming(users::PsUser& user, Attempt& attempt)
      -> Ended;
  /// @brief Runs Stage::access_route.
  ///
  /// @param attempt The run.
  /// @return The route's refusal when it has no owned entitlement and needs no
  ///     recheck; else std::nullopt.
  static auto take_the_access_route(Attempt& attempt) -> Ended;
  /// @brief Judges Stage::ownership: the recheck, the reloaded games and the
  /// route read again.
  ///
  /// @param attempt The run.
  /// @param check What the recheck answered.
  /// @return Check unavailable on failure; the not-owned refusal; not listed;
  ///     the new route's refusal; else std::nullopt.
  static auto judge_ownership(Attempt& attempt, OwnershipCheck check) -> Ended;
  /// @brief Judges Stage::entitlement.
  ///
  /// @param user The user.
  /// @param attempt The run.
  /// @param reread The entitlement read afresh.
  /// @return Check unavailable on failure; the refusal refusal_of gives when
  ///     the record is missing or no longer a streamable full game, after
  ///     remembering a StreamRefusal::Kind::trial_ended refusal for a trial
  ///     when ends_the_trial holds; else std::nullopt.
  auto judge_entitlement(
      users::PsUser& user, Attempt& attempt,
      Expected<std::optional<Ps5Entitlement>, Unanswered> reread) -> Ended;
  /// @brief Judges Stage::selected_title.
  ///
  /// Whether the resolver lists the product does not matter for an owned full
  /// game; only a failed or unasked request refuses.
  ///
  /// @param attempt The run.
  /// @param listing What the resolver answered.
  /// @return Check unavailable when the resolver failed or was not asked; else
  ///     std::nullopt.
  static auto judge_selected_title(
      Attempt& attempt, const catalog::cloud_stream::CloudGameListing& listing)
      -> Ended;
  /// @brief Judges Stage::exclusions.
  ///
  /// @param attempt The run.
  /// @param verdict The exclusion verdict.
  /// @return The verdict's refusal, or std::nullopt.
  static auto judge_exclusions(Attempt& attempt,
                               const CheckExclusion::Verdict& verdict) -> Ended;
  /// @brief Judges Stage::release, the last stage.
  ///
  /// @param user The user.
  /// @param attempt The run; its record must be set.
  /// @param unreleased The release refusal, when the release is not reached.
  /// @return Cleared through the re-read entitlement, or the release refusal.
  static auto judge_release(const users::PsUser& user, const Attempt& attempt,
                            std::optional<LaunchRefusal> unreleased)
      -> LaunchVerdict;

  /// @brief Looks the LegacyCloudStream entitlements up in the commerce list.
  ///
  /// @param user The user.
  /// @param client Sends the requests.
  /// @param ids The entitlement ids.
  /// @return The first held LegacyCloudStream record; std::nullopt when none is
  /// held, the
  ///     ids are empty, or the record found is not LegacyCloudStream; the
  ///     lookup's failure.
  [[nodiscard]] auto legacy_cloud_stream_record(
      users::PsUser& user, net::http::Client& client,
      std::span<const OwnershipId> ids)
      -> Expected<std::optional<OwnedRecord>, Unanswered>;
  /// @brief The held PS5 record of an entitlement.
  ///
  /// @param user The user.
  /// @param id The entitlement id.
  /// @return The record, or std::nullopt when no scan or record is held.
  auto held_record(const users::PsUser& user, const OwnershipId& id)
      -> std::optional<Ps5Entitlement>;
  /// @brief What to match exclusions against.
  ///
  /// @param record The entitlement record, when known.
  /// @param game The game.
  /// @return The record's title id and product, falling back to the game's
  ///     product; no title id without a record.
  static auto exclusion_subject(const std::optional<Ps5Entitlement>& record,
                                const MyGame& game) -> ExclusionSubject;
  /// @brief The cleared verdict launching through an owned entitlement.
  ///
  /// @param game The game.
  /// @param seen The games.
  /// @param user The user.
  /// @param owned The entitlement launched.
  /// @return Cleared, with the entitlement as identifier and holding, and
  ///     ClearedLaunch::trial set when the game is held as trial access.
  static auto cleared(const MyGame& game, const View& seen,
                      const users::PsUser& user, const OwnershipId& owned)
      -> LaunchVerdict;
  /// @brief Classifies a re-read entitlement whose owned check failed during
  /// selected-title preparation (20059).
  ///
  /// Reads the record's facts directly, in a fixed order, without relying
  /// on the order Ps5Entitlement::why_not_a_game checks them in.
  ///
  /// @param reread The singular re-read; std::nullopt when PS holds no
  ///     record of the entitlement.
  /// @return std::nullopt when the record is a streamable full game;
  ///     otherwise LaunchRefusal::invalid_title for no record or no product,
  ///     LaunchRefusal::title_taken_down when not streaming and inactive,
  ///     LaunchRefusal::streaming_unavailable when not streaming and active,
  ///     LaunchRefusal::purchase_needed when streaming but inactive or a
  ///     Play First Trial's play time is used up, and
  ///     LaunchRefusal::unexpected_entitlement_state for anything else.
  static auto refusal_of(const std::optional<Ps5Entitlement>& reread)
      -> std::optional<LaunchRefusal>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
  /// @brief Lists the user's games; not owned.
  ListGames& list_;
  /// @brief The shared checks; not owned.
  LaunchChecks& checks_;
  /// @brief Judges launches against the exclusion list.
  CheckExclusion exclusions_;
};

}  // namespace libchiaking::catalog::usecases::cloud_stream
