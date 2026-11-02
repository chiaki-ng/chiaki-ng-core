#include "src/core/consoles/adapters/titles/console_library.hpp"

#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "libchiaking/platform/runtime/time_units.hpp"
#include "src/core/psn/rest/gamelist/gamelist.hpp"
#include "src/core/utils/text/ascii.hpp"
#include "src/core/utils/text/integer.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <compare>
#include <iterator>
#include <utility>

namespace libchiaking::consoles {

ConsoleLibrary::ConsoleLibrary(KnownConsoles& known,
                               platform::HttpProvider& http,
                               platform::ThreadingProvider& threading,
                               const platform::ClockProvider& clock,
                               platform::DiagnosticsProvider* log,
                               std::size_t maximum_response_bytes)
    : known_(known),
      http_(http),
      clock_(clock),
      log_(log),
      maximum_response_bytes_(maximum_response_bytes),
      flights_(threading, clock),
      mutex_(threading) {}

auto ConsoleLibrary::asked(users::PsUser& user, const platform::StopToken& stop)
    -> Expected<Held, users::Unanswered> {
  const auto account = user.account_id();
  if (user.is_off_psn()) {
    forget(account);
    return unexpected(users::Unanswered{users::Unanswered::Reason::signed_out});
  }
  auto answer = flights_.answer(account.decimal(), stop,
                                [&] { return fetched(user, stop); });
  if (answer) {
    platform::Lock lock{mutex_};
    held_[account.value()] = answer.value();
  }
  return answer;
}

auto ConsoleLibrary::held_or_asked(users::PsUser& user,
                                   const platform::StopToken& stop)
    -> Expected<Held, users::Unanswered> {
  if (user.is_off_psn()) {
    forget(user.account_id());
    return unexpected(users::Unanswered{users::Unanswered::Reason::signed_out});
  }
  if (auto kept = held(user.account_id())) return kept;
  return asked(user, stop);
}

auto ConsoleLibrary::recently_played(users::PsUser& by,
                                     const platform::StopToken& stop)
    -> Expected<std::vector<RecentlyPlayed>, users::Unanswered> {
  const auto answer = asked(by, stop);
  if (!answer) return unexpected(answer.error());
  const auto& consoles = answer.value()->consoles;
  const auto links =
      AccountLink{known_, log_}.linked(by, link_candidates(*answer.value()));
  std::vector<RecentlyPlayed> recent;
  recent.reserve(links.size());
  for (const auto& link : links) {
    RecentlyPlayed shelf{link.console, {}};
    std::ranges::copy_if(consoles[link.listed].titles,
                         std::back_inserter(shelf.titles),
                         &InstalledTitle::recently_played);
    recent.push_back(std::move(shelf));
  }
  return recent;
}

auto ConsoleLibrary::shelf_of(Console& console, users::PsUser& of,
                              const platform::StopToken& stop)
    -> Expected<Shelf, TitlesUnavailable> {
  using Reason = TitlesUnavailable::Reason;
  if (console.generation() != Generation::ps5)
    return unexpected(TitlesUnavailable{Reason::ps5_only});
  if (of.is_off_psn()) {
    forget(of.account_id());
    return unexpected(TitlesUnavailable{
        Reason::unanswered,
        users::Unanswered{users::Unanswered::Reason::signed_out}});
  }
  if (!console.is_paired_with(of))
    return unexpected(TitlesUnavailable{Reason::not_paired});
  const auto answer = held_or_asked(of, stop);
  if (!answer)
    return unexpected(TitlesUnavailable{Reason::unanswered, answer.error()});
  const auto links =
      AccountLink{known_, log_}.linked(of, link_candidates(*answer.value()));
  const auto link = std::ranges::find(links, &console,
                                      [](const AccountLink::Link& candidate) {
                                        return candidate.console.get();
                                      });
  if (link == links.end())
    return unexpected(TitlesUnavailable{Reason::not_on_account});
  return answer.value()->consoles[link->listed];
}

auto ConsoleLibrary::fetched(users::PsUser& user,
                             const platform::StopToken& stop)
    -> Expected<Held, users::Unanswered> {
  net::http::Client http{http_, clock_, stop, log_};
  users::ConsoleCalls calls{user};
  const auto consoles = installed_list(http, calls);
  if (!consoles) return unexpected(consoles.error());

  TitleKeys installed;
  for (const auto& console : consoles.value())
    for (const auto& title : console.titles) installed.insert(title.key);

  const auto played = played_list(http, calls, installed);
  if (!played) return unexpected(played.error());

  auto answer = std::make_shared<Answer>();
  answer->consoles =
      shelved(consoles.value(), played.value().titles, played.value().recent);
  std::size_t total = 0;
  std::size_t timed = 0;
  for (const auto& shelf : answer->consoles) {
    total += shelf.titles.size();
    timed += static_cast<std::size_t>(
        std::ranges::count_if(shelf.titles, [](const InstalledTitle& title) {
          return title.last_used() &&
                 title.last_used()->known ==
                     InstalledTitle::LastUse::Known::on_this_console;
        }));
  }
  http.log(platform::LogSeverity::info,
           "console titles: " + std::to_string(answer->consoles.size()) +
               " consoles, " + std::to_string(total) + " titles, " +
               std::to_string(timed) + " with the console's own last use");
  return Held{std::move(answer)};
}

auto ConsoleLibrary::installed_list(net::http::Client& http,
                                    users::ConsoleCalls& calls)
    -> Expected<std::vector<ListedConsole>, users::Unanswered> {
  std::vector<ListedConsole> consoles;
  std::unordered_set<std::string> duids;
  std::size_t offset = 0;
  for (int page = 0; page < installed_page_limit; ++page) {
    auto listing =
        calls.installed_titles(http, offset, maximum_response_bytes_);
    if (!listing) return unexpected(listing.error());
    const auto received = listing.value().received;
    http.log(platform::LogSeverity::verbose,
             "console titles: console list page at offset " +
                 std::to_string(offset) + " returned " +
                 std::to_string(received) + " clients");
    if (received == 0) return consoles;
    bool added = false;
    for (auto& registered : listing.value().consoles) {
      auto console = listed(std::move(registered));
      if (console &&
          duids.insert(std::string{text::trim(console->duid)}).second) {
        consoles.push_back(std::move(*console));
        added = true;
      }
    }
    if (received < psn::rest::list_clients_page_size || !added) return consoles;
    offset += received;
  }
  http.log(platform::LogSeverity::warning,
           "console titles: console list page bound reached");
  return consoles;
}

auto ConsoleLibrary::played_list(net::http::Client& http,
                                 users::ConsoleCalls& calls,
                                 const TitleKeys& installed)
    -> Expected<PlayedList, users::Unanswered> {
  PlayedList list;
  std::size_t offset = 0;
  for (int page = 0; page < played_page_limit; ++page) {
    if (page > 0 && covered(installed, list.titles)) return list;
    auto played = calls.played_titles(http, offset, maximum_response_bytes_);
    if (!played) return unexpected(played.error());
    const auto received = played.value().received;
    http.log(platform::LogSeverity::verbose,
             "console titles: game list page at offset " +
                 std::to_string(offset) + " returned " +
                 std::to_string(received) + " titles");
    bool added = false;
    for (auto& title : played.value().titles) {
      const auto title_id = text::trim(title.title_id);
      if (title_id.empty()) continue;
      auto key = title_key(title_id);
      if (page == 0) list.recent.insert(key);
      Played row{std::string{title_id}, std::move(title.name),
                 std::move(title.image_url), std::move(title.category),
                 played_at(title.last_played)};
      if (list.titles.try_emplace(std::move(key), std::move(row)).second)
        added = true;
    }
    if (received < psn::rest::played_titles_page_size || !added) return list;
    offset += received;
  }
  if (!covered(installed, list.titles))
    http.log(platform::LogSeverity::verbose,
             "console titles: game list page bound reached");
  return list;
}

auto ConsoleLibrary::held(AccountId account) const -> Held {
  platform::Lock lock{mutex_};
  const auto found = held_.find(account.value());
  return found != held_.end() ? found->second : nullptr;
}

void ConsoleLibrary::forget(AccountId account) {
  platform::Lock lock{mutex_};
  held_.erase(account.value());
}

auto ConsoleLibrary::listed(psn::rest::RegisteredConsole registered)
    -> std::optional<ListedConsole> {
  if (text::trim(registered.duid).empty()) return std::nullopt;
  ListedConsole console{std::move(registered.duid),
                        std::move(registered.name),
                        {},
                        registered.device_uid
                            ? DeviceUid::from_bytes(*registered.device_uid)
                            : std::nullopt};
  TitleKeys seen;
  for (auto& title : registered.titles) {
    auto key = title_key(title.title_id);
    if (key.empty() || !seen.insert(key).second) continue;
    console.titles.push_back({std::move(key), std::move(title.platform),
                              concept_of(title.concept_id),
                              played_at(title.last_access)});
  }
  return console;
}

auto ConsoleLibrary::covered(const TitleKeys& installed,
                             const PlayedTitles& played) -> bool {
  return std::ranges::all_of(
      installed, [&](const std::string& key) { return played.contains(key); });
}

auto ConsoleLibrary::last_used(const InstalledTitle& title) noexcept
    -> std::optional<std::chrono::system_clock::time_point> {
  if (const auto& used = title.last_used()) return used->at;
  return std::nullopt;
}

auto ConsoleLibrary::concept_of(std::string_view text) -> std::string {
  const auto digits = text::trim(text);
  return digits.size() <= InstalledTitle::concept_limit &&
                 std::ranges::all_of(digits, text::ascii_digit)
             ? std::string{digits}
             : std::string{};
}

auto ConsoleLibrary::before(const InstalledTitle& left,
                            const InstalledTitle& right) noexcept -> bool {
  if (left.recently_played() != right.recently_played())
    return left.recently_played();
  if (left.recently_played() && last_used(left) != last_used(right))
    return last_used(left) > last_used(right);
  const auto folded = text::casecmp(left.name(), right.name());
  if (std::is_neq(folded)) return std::is_lt(folded);
  if (left.name() != right.name()) return left.name() < right.name();
  return left.title_id() < right.title_id();
}

auto ConsoleLibrary::link_candidates(const Answer& answer)
    -> std::vector<AccountLink::Listed> {
  std::vector<AccountLink::Listed> candidates;
  candidates.reserve(answer.consoles.size());
  for (const auto& shelf : answer.consoles)
    candidates.push_back(AccountLink::Listed{shelf.name, shelf.device_uid});
  return candidates;
}

auto ConsoleLibrary::title_key(std::string_view title_id) -> std::string {
  const auto trimmed = text::trim(title_id);
  std::string key;
  key.reserve(trimmed.size());
  for (const char ch : trimmed) key.push_back(text::ascii_upper(ch));
  return key;
}

auto ConsoleLibrary::start_id(std::string_view title_id) -> std::string {
  auto trimmed = text::trim(title_id);
  constexpr std::string_view suffix = "_00";
  if (trimmed.ends_with(suffix)) trimmed.remove_suffix(suffix.size());
  return std::string{trimmed};
}

auto ConsoleLibrary::played_at(std::string_view text) -> std::optional<Time> {
  const auto instant = json::ps::Instant::from_text(text::trim(text));
  if (!instant || instant->time() <= Time{}) return std::nullopt;
  return instant->time();
}

auto ConsoleLibrary::is_app(std::string_view category) -> bool {
  constexpr std::string_view suffix = "_app";
  const auto trimmed = text::trim(category);
  return trimmed.size() >= suffix.size() &&
         text::ieq(trimmed.substr(trimmed.size() - suffix.size()), suffix);
}

auto ConsoleLibrary::shelved(const std::vector<ListedConsole>& consoles,
                             const PlayedTitles& played,
                             const TitleKeys& recent) -> std::vector<Shelf> {
  std::vector<Shelf> shelves;
  shelves.reserve(consoles.size());
  for (const auto& console : consoles) {
    Shelf shelf{console.duid, console.name, console.device_uid, {}};
    for (const auto& installed : console.titles) {
      const auto found = played.find(installed.key);
      if (found == played.end() || is_app(found->second.category)) continue;
      const auto& row = found->second;
      InstalledTitle::Facts facts{
          row.title_id, start_id(row.title_id), row.name, row.cover_url,
          text::ieq(text::trim(installed.platform), "PS4") ? Generation::ps4
                                                           : Generation::ps5};
      using Known = InstalledTitle::LastUse::Known;
      if (installed.last_access)
        facts.last_used = InstalledTitle::LastUse{*installed.last_access,
                                                  Known::on_this_console};
      else if (row.last_played)
        facts.last_used =
            InstalledTitle::LastUse{*row.last_played, Known::on_the_account};
      facts.recently_played = recent.contains(installed.key);
      facts.concept_id = installed.concept_id;
      if (auto title = InstalledTitle::create(std::move(facts)))
        shelf.titles.push_back(std::move(*title));
    }
    std::ranges::stable_sort(shelf.titles, before);
    shelves.push_back(std::move(shelf));
  }
  return shelves;
}

}  // namespace libchiaking::consoles
