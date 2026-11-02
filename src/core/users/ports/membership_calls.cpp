#include "src/core/users/ports/membership_calls.hpp"

#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/psn/graphql/authenticated/client.hpp"
#include "src/core/psn/session_internals.hpp"
#include "src/core/users/ports/authorised_request.hpp"
#include "src/core/utils/json/json.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <utility>

namespace libchiaking::users {

MembershipCalls::MembershipCalls(PsUser& user) noexcept
    : user_(PsUser::Core::of(user)) {}

auto MembershipCalls::membership(net::http::Client& http, Tier remembered_from)
    -> MembershipCheck {
  return AuthorisedRequest{user_}.membership(http, remembered_from);
}

auto MembershipCalls::answered_membership(net::http::Client& http,
                                          Tier remembered_from)
    -> Expected<MembershipAnswer, Unanswered> {
  return AuthorisedRequest{user_}.answered_membership(http, remembered_from);
}

void MembershipCalls::membership_rejected_by_service() {
  AuthorisedRequest{user_}.membership_rejected_by_service();
}

auto MembershipCalls::remembered_membership()
    -> std::optional<MembershipAnswer> {
  return user_.memberships().remembered();
}

auto MembershipCalls::membership_json(const MembershipAnswer& answer,
                                      std::optional<bool> fresh)
    -> std::string {
  const auto& plan = answer.membership.plan();
  const auto written = json::ps::write(MembershipDocument{
      .status =
          psn::graphql::CheckError::status_name(psn::graphql::CheckStatus::ok),
      .tier = Membership::name_of(answer.membership.tier()),
      .tier_label = text::c_string(plan.tier_label),
      .sku_id = text::c_string(plan.sku_id),
      .product_name = text::c_string(plan.product_name),
      .end_date = text::c_string(plan.end_date),
      .active = answer.membership.is_ps_plus_member().value_or(false),
      .cached = fresh ? std::nullopt : std::optional{answer.cached},
      .fetched_at_ms = answer.fetched_at_ms,
      .fresh = fresh});
  return written ? std::string{written.value().view()} : std::string{};
}

}  // namespace libchiaking::users
