#include "src/core/consoles/adapters/pairing/account_link.hpp"

#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/known_consoles.hpp"
#include "libchiaking/platform/diagnostics/provider.hpp"
#include "src/core/consoles/adapters/roster/roster.hpp"
#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <iterator>
#include <utility>

namespace libchiaking::consoles {

AccountLink::AccountLink(KnownConsoles& known,
                         platform::DiagnosticsProvider* log) noexcept
    : facts_(known), log_(log) {}

AccountLink::AccountLink(Roster& roster,
                         platform::DiagnosticsProvider* log) noexcept
    : facts_(roster), log_(log) {}

auto AccountLink::linked(const users::PsUser& user,
                         std::span<const Listed> listed) -> std::vector<Link> {
  std::vector<std::shared_ptr<Console>> paired;
  std::ranges::copy_if(facts_.roster().everything(), std::back_inserter(paired),
                       [&user](const std::shared_ptr<Console>& console) {
                         return console->generation() == Generation::ps5 &&
                                !console->is_forgotten() &&
                                console->is_paired_with(user);
                       });
  std::vector<Saved> saved;
  saved.reserve(paired.size());
  for (const auto& console : paired)
    saved.push_back(
        Saved{console->name(), facts_.device_uid_of(*console, user)});

  std::vector<Link> links;
  for (const auto& match : matched(saved, listed)) {
    const auto& console = paired[match.saved];
    const auto& ps = listed[match.listed];
    if (match.rule != Rule::stored_device_uid && ps.device_uid &&
        !facts_.linked(*console, user, *ps.device_uid) && log_ != nullptr)
      log_->write({platform::LogSeverity::warning,
                   "consoles: a console's account link was not saved"});
    if (const auto name =
            ConsoleName::create(ps.name, ConsoleName::Source::account))
      facts_.named(*console, *name);
    links.push_back(Link{console, match.listed, match.rule});
  }
  return links;
}

auto AccountLink::matched(std::span<const Saved> saved,
                          std::span<const Listed> listed)
    -> std::vector<Match> {
  std::vector<Match> matches;
  std::vector<bool> saved_taken(saved.size());
  std::vector<bool> listed_taken(listed.size());
  const auto take = [&](std::size_t saved_index, std::size_t listed_index,
                        Rule rule) {
    saved_taken[saved_index] = true;
    listed_taken[listed_index] = true;
    matches.push_back(Match{saved_index, listed_index, rule});
  };
  const auto sole_hit = [&](const auto& hits) -> std::optional<std::size_t> {
    std::optional<std::size_t> hit;
    for (std::size_t index = 0; index < listed.size(); ++index) {
      if (listed_taken[index] || !hits(listed[index])) continue;
      if (hit) return std::nullopt;
      hit = index;
    }
    return hit;
  };

  for (std::size_t index = 0; index < saved.size(); ++index) {
    const auto& stored = saved[index].device_uid;
    if (!stored || std::ranges::count(saved, stored, &Saved::device_uid) != 1)
      continue;
    if (const auto hit =
            sole_hit([&](const Listed& ps) { return ps.device_uid == stored; }))
      take(index, *hit, Rule::stored_device_uid);
  }

  for (std::size_t index = 0; index < saved.size(); ++index) {
    if (saved[index].device_uid || saved_taken[index]) continue;
    const auto name = folded(saved[index].name);
    if (name.empty()) continue;
    std::size_t namesakes = 0;
    for (std::size_t other = 0; other < saved.size(); ++other)
      if (!saved[other].device_uid && !saved_taken[other] &&
          folded(saved[other].name) == name)
        ++namesakes;
    if (namesakes != 1) continue;
    if (const auto hit =
            sole_hit([&](const Listed& ps) { return folded(ps.name) == name; }))
      take(index, *hit, Rule::unique_name);
  }

  if (saved.size() == 1 && listed.size() == 1 && matches.empty() &&
      !saved.front().device_uid)
    take(0, 0, Rule::sole_console);
  return matches;
}

auto AccountLink::listed(std::span<const AccountConsole> consoles)
    -> std::vector<Listed> {
  std::vector<Listed> listed;
  listed.reserve(consoles.size());
  for (const auto& console : consoles)
    listed.push_back(Listed{console.name(), console.device_uid()});
  return listed;
}

auto AccountLink::folded(std::string_view name) -> std::string {
  const auto trimmed = text::trim(name);
  std::string folded;
  folded.reserve(trimmed.size());
  std::ranges::transform(trimmed, std::back_inserter(folded),
                         text::ascii_lower);
  return folded;
}

}  // namespace libchiaking::consoles
