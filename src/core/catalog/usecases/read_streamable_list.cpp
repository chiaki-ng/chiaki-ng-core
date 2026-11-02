#include "src/core/catalog/usecases/read_streamable_list.hpp"

#include "src/core/catalog/adapters/ps/old_service_shelves.hpp"
#include "src/core/catalog/adapters/ps/ps5_store_lists.hpp"
#include "src/core/catalog/adapters/ps/public_store_walk.hpp"
#include "src/core/catalog/adapters/ps/streaming_copies.hpp"
#include "src/core/catalog/usecases/asker.hpp"
#include "src/core/users/ports/catalog_calls.hpp"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace libchiaking::catalog::usecases {

ReadStreamableList::ReadStreamableList(
    platform::HttpProvider& http, const platform::ClockProvider& clock,
    const platform::ClockProvider& wall_clock,
    platform::DiagnosticsProvider* log, HeldAnswers& held) noexcept
    : http_(http), clock_(clock), wall_(wall_clock), log_(log), held_(held) {}

auto ReadStreamableList::list(users::PsUser& user, const Stop& stop,
                              const FetchProgress& progress)
    -> Expected<ListPtr, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  const auto held_in_region = [&]() -> ListPtr {
    const auto region = Asker::region_of(user);
    return region ? held_.fresh_list(*region) : nullptr;
  };
  auto& in_flight = held_.in_flight().lists;
  return HeldAnswers::answer(
      in_flight, HeldAnswers::Question::by(user.account_id()), stop,
      held_in_region, [&]() -> Expected<ListPtr, Unanswered> {
        const auto region = Asker::region_of(user);
        if (!region) return walked_list(user, stop, progress);
        return HeldAnswers::answer(
            in_flight, HeldAnswers::Question::listed_in(*region), stop,
            held_in_region, [&] { return walked_list(user, stop, progress); });
      });
}

auto ReadStreamableList::walked_list(users::PsUser& user, const Stop& stop,
                                     const FetchProgress& progress)
    -> Expected<ListPtr, Unanswered> {
  net::http::Client client{http_, clock_, stop, log_};
  const auto settled = users::CatalogCalls{user}.settle_region();
  if (const auto region =
          settled ? StoreLocale::for_account(*settled) : std::nullopt) {
    progress.doing(FetchActivity::walking_public_store);
    auto anonymous =
        ps::PublicStoreWalk{client, wall_}.walk(*region, progress.pages());
    if (!anonymous && anonymous.error().is_cancelled())
      return unexpected(anonymous.error());
    if (anonymous && !anonymous.value().entries().empty())
      return listed(user, client, std::move(anonymous), progress);
  }
  progress.doing(FetchActivity::walking_old_service_shelves);
  ps::OldServiceShelves shelves{user, client, wall_};
  auto walked = shelves.walk(progress.pages());
  if (!walked && walked.error().is_cancelled())
    return unexpected(walked.error());
  held_.note_access(user, shelves.access_shown_by(walked));
  return listed(user, client, std::move(walked), progress);
}

auto ReadStreamableList::listed(users::PsUser& user, net::http::Client& client,
                                Expected<RawListing, Unanswered> walked,
                                const FetchProgress& progress)
    -> Expected<ListPtr, Unanswered> {
  if (!walked && walked.error().needs_sign_in())
    return unexpected(walked.error());
  const auto region = walked ? std::optional{walked.value().source().region}
                             : Asker::region_of(user);
  if (!region) return unexpected(walked.error());
  auto origin = ListOrigin::account_service;
  bool complete = true;
  auto settled = *region;
  std::vector<ListedEntry> entries;
  if (walked) {
    origin = walked.value().source().origin;
    complete = walked.value().source().complete;
    settled = walked.value().source().settled_locale;
    entries.assign(walked.value().entries().begin(),
                   walked.value().entries().end());
  } else {
    origin = ListOrigin::public_store;
    progress.doing(FetchActivity::walking_public_store_fallback);
    const auto walked_publicly =
        ps::PublicStoreWalk{client, wall_}.walk(*region, progress.pages());
    if (!walked_publicly && walked_publicly.error().is_cancelled())
      return unexpected(walked_publicly.error());
    complete =
        walked_publicly && walked_publicly.value().source().complete &&
        walked.error().reason() == Unanswered::Reason::region_unsupported;
    if (walked_publicly && !walked_publicly.value().entries().empty())
      origin = walked_publicly.value().source().origin;
    if (walked_publicly)
      entries.assign(walked_publicly.value().entries().begin(),
                     walked_publicly.value().entries().end());
  }
  progress.doing(FetchActivity::checking_streaming_copies);
  const auto copies =
      mark_copies_no_plan_covers(user, client, entries, progress.pages());
  progress.doing(FetchActivity::reading_ps5_store_lists);
  const auto on_ps5 = ps::Ps5StoreLists{client, wall_}.fetch(*region);
  if (!on_ps5 && on_ps5.error().is_cancelled())
    return unexpected(on_ps5.error());
  if (on_ps5) {
    settled = on_ps5.value().source().settled_locale;
    entries.insert(entries.end(), on_ps5.value().entries().begin(),
                   on_ps5.value().entries().end());
  }
  complete = complete && on_ps5 && on_ps5.value().source().complete;
  if (entries.empty())
    return unexpected(walked ? Unanswered{Unanswered::Reason::unavailable}
                             : walked.error());

  RawListing merged{
      {*region, std::move(settled), origin, complete, held_.now(), copies}};
  for (auto& entry : entries) merged.add(std::move(entry));
  auto filtered = merged.filter_raw_streamable();
  if (!filtered) return unexpected(Unanswered{Unanswered::Reason::malformed});
  auto list =
      std::make_shared<const StreamableList>(std::move(filtered).value());
  held_.keep(*region, list);
  return list;
}

auto ReadStreamableList::mark_copies_no_plan_covers(
    const users::PsUser& user, net::http::Client& client,
    std::vector<ListedEntry>& entries, PageCount pages)
    -> std::optional<StreamableList::CopiesChecked> {
  const auto audience = Asker::audience_of(user);
  if (!audience) return std::nullopt;
  std::vector<ProductId> on_ps4;
  for (const auto& entry : entries)
    if (entry.product.platform() == Platform::ps4)
      on_ps4.push_back(entry.product);
  const auto copies = ps::StreamingCopies{client}.of(
      on_ps4, audience->locale, audience->age, std::move(pages));
  for (auto& entry : entries)
    if (copies.without_a_covered_copy.contains(entry.product))
      entry.streaming_supported = false;
  return StreamableList::CopiesChecked{copies.asked, copies.answered()};
}

}  // namespace libchiaking::catalog::usecases
