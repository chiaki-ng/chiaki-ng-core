/// @file
/// @brief Declares MembershipCalls, how a user's PlayStation Plus membership is
/// asked of PSN and remembered.
#pragma once

#include "src/core/net/transport/http/client.hpp"
#include "src/core/users/adapters/membership_cache.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <libchiaking/detail/users/ps_user.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::users {

/// @brief A user's PlayStation Plus membership, asked of PSN with the app
/// token and shared between concurrent callers.
///
/// The users context's own port, which PsUser::get_my_membership reads. The
/// questions themselves live in AuthorisedRequest, which CatalogCalls and
/// PlayCalls also forward to.
///
/// A user that is off PSN is answered at once and nothing is sent. A failure
/// that means PSN no longer accepts the NPSSO marks the user's sign-in as
/// refused, so the user reports SignInState::expired.
///
/// The object is a short-lived view: it only refers to the user's core, holds
/// no state of its own, and is made afresh wherever a call is needed. It must
/// not outlive the PsUser it was made for. Every call blocks the calling
/// thread for its requests, each bounded by the HTTP client's timeouts and its
/// stop token; calls may be made from several threads at once, the user's core
/// providing the locking.
class MembershipCalls {
 public:
  /// @brief Views a user handed out by SignedInUsers.
  ///
  /// @param user The user; must outlive this object.
  explicit MembershipCalls(PsUser& user) noexcept;
  /// @brief Views a user's core directly, for code inside the users
  /// context.
  ///
  /// @param user The user's core; must outlive this object.
  explicit MembershipCalls(PsUser::Core& user) noexcept : user_(user) {}

  /// @brief Reads the user's PlayStation Plus membership through the GraphQL
  /// subscriptions query, reusing a remembered answer when it will do.
  ///
  /// The highest tier among the active subscriptions wins; no active
  /// subscription means Tier::none. Concurrent readers share one request
  /// through MembershipCache.
  ///
  /// @param http The client the query is sent through; its stop token
  ///     cancels it.
  /// @param remembered_from The lowest tier a remembered answer must show to
  ///     be reused; Tier::none reuses any fresh answer.
  /// @return The membership.
  /// @retval psn::graphql::CheckStatus::no_session The user is off PSN.
  /// @retval psn::graphql::GraphqlError::Kind::malformed An active subscription
  ///     has an unknown tier label, or its SKU names a different tier.
  /// @retval psn::graphql::CheckError Any other failure of the query.
  [[nodiscard]] auto membership(net::http::Client& http,
                                Tier remembered_from = Tier::none)
      -> MembershipCheck;

  /// @brief Finds out the user's PlayStation Plus membership, answering with
  /// what the profile showed when PSN cannot be asked.
  ///
  /// The app token is restored first unless the sign-in has expired; a
  /// failure to restore it is not reported itself, except cancellation. Then
  /// membership() is asked; concurrent callers with the same
  /// `remembered_from` share one attempt. When the query fails but the last
  /// profile showed the user to be a member, that answer is returned, not
  /// cached and stamped now, with the tier unknown.
  ///
  /// @param http The client the requests are sent through; its stop token
  ///     cancels them.
  /// @param remembered_from The lowest tier a remembered answer must show to
  ///     be reused.
  /// @return The membership and when it was learned.
  /// @retval Unanswered::Reason::signed_out The user is off PSN.
  /// @retval Unanswered::Reason::cancelled The stop token fired.
  /// @retval Unanswered::Reason::store_access_lapsed The app token is
  ///     missing, expired or was refused.
  /// @retval Unanswered::Reason::rejected PSN rejected the query or found
  ///     nothing.
  /// @retval Unanswered::Reason::malformed The reply could not be read.
  /// @retval Unanswered::Reason::unavailable Any other failure.
  [[nodiscard]] auto answered_membership(net::http::Client& http,
                                         Tier remembered_from = Tier::none)
      -> Expected<MembershipAnswer, Unanswered>;

  /// @brief Returns the remembered membership while it is fresh, without
  /// asking PSN.
  ///
  /// @return The membership, or nothing when none fresh is held.
  [[nodiscard]] auto remembered_membership() -> std::optional<MembershipAnswer>;

  /// @brief Forgets the remembered membership after PS refused a cloud
  /// server reservation for want of PlayStation Plus, so the next question
  /// asks PSN again.
  void membership_rejected_by_service();

  /// @brief Writes a membership answer as JSON for diagnostics.
  ///
  /// @param answer The answer.
  /// @param fresh Whether a held answer is still fresh, when describing the
  ///     cache; nothing to report the answer's own `cached` flag instead.
  /// @return Compact JSON with the tier, plan, activity and fetch time; empty
  ///     text if writing failed.
  static auto membership_json(const MembershipAnswer& answer,
                              std::optional<bool> fresh = std::nullopt)
      -> std::string;

 private:
  /// @brief The shape membership_json() writes, with camelCase keys.
  struct MembershipDocument {
    /// @brief The JSON key style.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The check status, always `ok`.
    std::string_view status;
    /// @brief The tier's name.
    std::string_view tier;
    /// @brief PS's tier label for the plan.
    std::string_view tier_label;
    /// @brief The plan's SKU.
    std::string_view sku_id;
    /// @brief The plan's product name.
    std::string_view product_name;
    /// @brief When the plan ends, as PS wrote it.
    std::string_view end_date;
    /// @brief Whether the user is a PlayStation Plus member; false when
    /// unknown.
    bool active{};
    /// @brief Whether the answer came from the cache; omitted when fresh
    /// is reported.
    std::optional<bool> cached;
    /// @brief When the answer was fetched, in milliseconds since the Unix
    /// epoch by the wall clock.
    std::int64_t fetched_at_ms{};
    /// @brief Whether a held answer is still fresh; omitted when not
    /// describing the cache.
    std::optional<bool> fresh;
  };

  /// @brief The user the calls are made for.
  PsUser::Core& user_;
};

}  // namespace libchiaking::users
