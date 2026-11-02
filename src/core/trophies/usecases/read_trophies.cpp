#include "src/core/trophies/usecases/read_trophies.hpp"

#include <algorithm>
#include <cstddef>
#include <format>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace libchiaking::trophies::usecases {

ReadTrophies::ReadTrophies(Providers providers, users::RequestPacer& pacer,
                           Freshness freshness)
    : http_(providers.http),
      clock_(providers.clock),
      waiters_(std::move(providers.waiters)),
      log_(providers.diagnostics),
      freshness_(freshness),
      records_(providers.records, providers.wall_clock),
      pacer_(pacer),
      summaries_(providers.threading, providers.clock),
      titles_(providers.threading, providers.clock),
      details_(providers.threading, providers.clock),
      sets_(providers.threading, providers.clock) {}

auto ReadTrophies::turned_away(const users::PsUser& user)
    -> std::optional<users::Unanswered> {
  using Reason = users::Unanswered::Reason;
  switch (user.sign_in_state()) {
    case users::SignInState::signed_out:
      return users::Unanswered{Reason::signed_out};
    case users::SignInState::expired:
      return users::Unanswered{Reason::sign_in_expired};
    case users::SignInState::signed_in:
    case users::SignInState::store_access_lapsed:
      break;
  }
  return std::nullopt;
}

auto ReadTrophies::question(std::string_view kind, AccountId account,
                            std::string_view about) -> std::string {
  if (about.empty()) return std::format("{}/{}", kind, account.value());
  return std::format("{}/{}/{}", kind, account.value(), about);
}

auto ReadTrophies::joined(std::span<const NpTitleId> titles) -> std::string {
  std::string ids;
  for (const auto& title : titles) {
    if (!ids.empty()) ids += ',';
    ids += title.text();
  }
  return ids;
}

auto ReadTrophies::client(const Stop& stop, net::http::Waiter& waiter)
    -> net::http::Client {
  return net::http::Client{http_, clock_, stop, log_, &waiter};
}

auto ReadTrophies::summary(users::PsUser& user, const Stop& stop)
    -> Expected<TrophySummary, users::Unanswered> {
  if (auto refused = turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  if (auto kept = records_.summary(account);
      kept && freshness_.fresh(kept->written_at, records_.now()))
    return std::move(kept->value);
  return summaries_.answer(question("summary", account), stop, [&] {
    auto waiter = waiters_();
    auto http = client(stop, *waiter);
    auto read = ps::TrophyRequests{http, user, pacer_}.summary();
    if (read) records_.keep(account, *read);
    return read;
  });
}

auto ReadTrophies::titles(users::PsUser& user, const Stop& stop)
    -> Expected<std::vector<TrophyTitle>, users::Unanswered> {
  if (auto refused = turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  if (auto kept = records_.titles(account);
      kept && freshness_.fresh(kept->written_at, records_.now()))
    return std::move(kept->value);
  return titles_.answer(question("titles", account), stop, [&] {
    auto waiter = waiters_();
    auto http = client(stop, *waiter);
    auto read = ps::TrophyRequests{http, user, pacer_}.titles();
    if (read) records_.keep(account, *read);
    return read;
  });
}

auto ReadTrophies::trophies(users::PsUser& user, const TrophyTitle& title,
                            const Stop& stop)
    -> Expected<TitleTrophies, users::Unanswered> {
  if (auto refused = turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  if (auto kept = records_.trophies(account, title.id);
      kept && freshness_.still_matches(kept->value, kept->written_at, title,
                                       records_.now()))
    return std::move(kept->value);
  return details_.answer(
      question("title", account, title.id.text()), stop, [&] {
        auto waiter = waiters_();
        auto http = client(stop, *waiter);
        auto read = ps::TrophyRequests{http, user, pacer_}.trophies_of(title);
        if (read) records_.keep(account, *read);
        return read;
      });
}

auto ReadTrophies::title_sets(users::PsUser& user,
                              std::span<const NpTitleId> titles,
                              const Stop& stop)
    -> Expected<std::vector<TitleSets>, users::Unanswered> {
  if (auto refused = turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  std::vector<NpTitleId> distinct;
  for (const auto& title : titles)
    if (!std::ranges::contains(distinct, title)) distinct.push_back(title);
  std::map<NpTitleId, TitleSets> answered;
  std::vector<NpTitleId> asked;
  for (const auto& title : distinct) {
    auto kept = records_.title_sets(account, title);
    if (kept && freshness_.fresh(kept->written_at, records_.now()))
      answered.insert_or_assign(title, std::move(kept->value));
    else
      asked.push_back(title);
  }
  constexpr auto per_request = ps::TrophyRequests::titles_per_request;
  const std::span pending{asked};
  for (std::size_t from = 0; from < pending.size(); from += per_request) {
    const auto batch =
        pending.subspan(from, std::min(per_request, pending.size() - from));
    auto read =
        sets_.answer(question("sets", account, joined(batch)), stop, [&] {
          auto waiter = waiters_();
          auto http = client(stop, *waiter);
          auto found = ps::TrophyRequests{http, user, pacer_}.title_sets(batch);
          if (found)
            for (const auto& sets : *found) records_.keep(account, sets);
          return found;
        });
    if (!read) return unexpected(read.error());
    for (auto& sets : *read) {
      auto title = sets.title;
      answered.insert_or_assign(std::move(title), std::move(sets));
    }
  }
  std::vector<TitleSets> ordered;
  ordered.reserve(distinct.size());
  for (const auto& title : distinct)
    if (auto found = answered.find(title); found != answered.end())
      ordered.push_back(std::move(found->second));
  return ordered;
}

auto ReadTrophies::current(users::PsUser& user, const NpTitleId& title,
                           const Stop& stop)
    -> Expected<std::optional<CurrentTrophies>, users::Unanswered> {
  if (auto refused = turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  auto waiter = waiters_();
  auto http = client(stop, *waiter);
  auto found = ps::TrophyRequests{http, user, pacer_}.release(title);
  if (!found) return unexpected(found.error());
  records_.keep(account, found->sets);
  if (!found->title) return std::optional<CurrentTrophies>{};
  auto read = trophies(user, *found->title, stop);
  if (!read) return unexpected(read.error());
  return std::optional<CurrentTrophies>{
      CurrentTrophies{std::move(*found->title), std::move(*read)}};
}

void ReadTrophies::start_over(AccountId account) { records_.forget(account); }

void ReadTrophies::forget(AccountId account) {
  records_.forget(account);
  pacer_.forget(account);
}

}  // namespace libchiaking::trophies::usecases
