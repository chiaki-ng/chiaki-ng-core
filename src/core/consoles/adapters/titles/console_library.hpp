/// @file
/// @brief Declares ConsoleLibrary, which reads from PSN what is installed on
/// each of the account's PS5 consoles and what the account played recently,
/// and joins the two into the shelves a frontend shows.
///
/// PSN's console directory lists each console's installed title ids, and the
/// game list names the titles the account has played. A title is shown only
/// when the game list names it, so its name and cover are known; apps are
/// left out.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/console_titles.hpp"
#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/consoles/adapters/pairing/account_link.hpp"
#include "src/core/net/transport/http/client.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/users/ports/console_calls.hpp"
#include "src/core/utils/concurrency/shared_flight.hpp"
#include "src/core/utils/json/ps_fields.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::consoles {

class KnownConsoles;

/// @brief The installed games of a user's PS5 consoles, held per account.
///
/// asked() always reads PSN afresh; concurrent askers for the same account
/// share one read. The latest answer for each account is held until the
/// user is found off PSN, and held_or_asked() reuses it. Each answer's
/// consoles are linked to the user's paired consoles through AccountLink.
///
/// Safe to use from several threads at once. Every read blocks for its PSN
/// requests, each bounded by the HTTP client's timeouts and the caller's
/// stop token. The registry and providers must outlive it.
class ConsoleLibrary {
 public:
  /// @brief Most pages of the console directory read in one answer.
  static constexpr int installed_page_limit = 50;
  /// @brief Most pages of the game list read in one answer.
  static constexpr int played_page_limit = 10;

  /// @brief A moment as PS writes it, on the wall clock.
  using Time = json::ps::Instant::Time;

  /// @brief A title the console directory lists as installed on a console.
  struct Installed {
    /// @brief The title id, trimmed and uppercased; see title_key().
    std::string key;
    /// @brief The platform PS gives, such as `PS4` or `PS5`.
    std::string platform;
    /// @brief The concept id when it is all digits and short enough; empty
    /// otherwise.
    std::string concept_id{};
    /// @brief When the title was last used on this console; empty when
    /// unknown.
    std::optional<Time> last_access{};
  };

  /// @brief A console the directory lists, with its installed titles.
  struct ListedConsole {
    /// @brief The console's device uid as PS writes it; never blank.
    std::string duid;
    /// @brief The name PSN holds for the console.
    std::string name;
    /// @brief The installed titles, each key once, in PS's order.
    std::vector<Installed> titles;
    /// @brief The device uid, when PS's text could be read as one.
    std::optional<DeviceUid> device_uid{};
  };

  /// @brief A title from the account's game list.
  struct Played {
    /// @brief The title id, trimmed.
    std::string title_id;
    /// @brief The title's display name.
    std::string name;
    /// @brief The address of its cover image.
    std::string cover_url;
    /// @brief PS's category, such as `ps5_native_game` or one ending in
    /// `_app`.
    std::string category;
    /// @brief When the account last played it; empty when unknown.
    std::optional<Time> last_played{};
  };

  /// @brief Played titles by title key.
  using PlayedTitles = std::unordered_map<std::string, Played>;
  /// @brief A set of title keys.
  using TitleKeys = std::unordered_set<std::string>;

  /// @brief One console's games, ready to show.
  struct Shelf {
    /// @brief The console's device uid as PS writes it.
    std::string duid;
    /// @brief The name PSN holds for the console.
    std::string name;
    /// @brief The device uid, when readable.
    std::optional<DeviceUid> device_uid{};
    /// @brief The installed games, recently played first (most recent
    /// first), then by name ignoring case.
    std::vector<InstalledTitle> titles;
  };

  /// @brief Everything one read of PSN learned.
  struct Answer {
    /// @brief One shelf per listed console, in PS's order.
    std::vector<Shelf> consoles;
  };

  /// @brief A shared, immutable answer.
  using Held = std::shared_ptr<const Answer>;

  /// @brief Makes a library with nothing held.
  ///
  /// @param known The registry whose consoles are linked to listed ones; must
  ///     outlive this object.
  /// @param http The HTTP provider; must outlive this object.
  /// @param threading Makes the library's lock and the waits askers share a
  ///     read through; must outlive this object.
  /// @param clock The monotonic clock those waits measure on; must outlive
  ///     this object.
  /// @param log Where requests and counts are logged; null logs nothing. Must
  ///     outlive this object.
  /// @param maximum_response_bytes Largest reply accepted per page, in bytes.
  ConsoleLibrary(KnownConsoles& known, platform::HttpProvider& http,
                 platform::ThreadingProvider& threading,
                 const platform::ClockProvider& clock,
                 platform::DiagnosticsProvider* log = nullptr,
                 std::size_t maximum_response_bytes =
                     net::http::default_maximum_response_bytes);
  /// @brief Not copyable.
  ConsoleLibrary(const ConsoleLibrary&) = delete;
  /// @brief Not copyable.
  auto operator=(const ConsoleLibrary&) -> ConsoleLibrary& = delete;

  /// @brief Reads the user's consoles and games from PSN and holds the
  /// answer.
  ///
  /// @param user The user.
  /// @param stop Cancels the requests.
  /// @return The answer.
  /// @retval users::Unanswered::Reason::signed_out The user is off PSN; what
  ///     was held for the account is dropped.
  /// @retval users::Unanswered Any failure of the PSN requests.
  [[nodiscard]] auto asked(users::PsUser& user, const platform::StopToken& stop)
      -> Expected<Held, users::Unanswered>;
  /// @brief Returns the answer held for the user, reading PSN only when none
  /// is held.
  ///
  /// @param user The user.
  /// @param stop Cancels the requests.
  /// @return The answer.
  /// @retval users::Unanswered As asked().
  [[nodiscard]] auto held_or_asked(users::PsUser& user,
                                   const platform::StopToken& stop)
      -> Expected<Held, users::Unanswered>;
  /// @brief Reads PSN afresh and lists, for each paired console linked to a
  /// listed one, the installed games the account played most recently.
  ///
  /// @param by The user.
  /// @param stop Cancels the requests.
  /// @return One entry per linked console, its titles those on the first page
  ///     of the game list, in shelf order.
  /// @retval users::Unanswered As asked().
  [[nodiscard]] auto recently_played(users::PsUser& by,
                                     const platform::StopToken& stop)
      -> Expected<std::vector<RecentlyPlayed>, users::Unanswered>;
  /// @brief The shelf of one paired PS5, from the held answer or a fresh
  /// read.
  ///
  /// @param console The console.
  /// @param of The user.
  /// @param stop Cancels the requests.
  /// @return The console's shelf.
  /// @retval TitlesUnavailable::Reason::ps5_only The console is a PS4.
  /// @retval TitlesUnavailable::Reason::unanswered The user is off PSN, or
  ///     PSN could not be read; the failure is carried.
  /// @retval TitlesUnavailable::Reason::not_paired The user is not paired
  ///     with the console.
  /// @retval TitlesUnavailable::Reason::not_on_account The console could not
  ///     be matched to any console PSN lists.
  [[nodiscard]] auto shelf_of(Console& console, users::PsUser& of,
                              const platform::StopToken& stop)
      -> Expected<Shelf, TitlesUnavailable>;

  /// @brief The key installed and played titles are matched by.
  ///
  /// @param title_id A title id.
  /// @return The id trimmed and ASCII-uppercased.
  static auto title_key(std::string_view title_id) -> std::string;
  /// @brief The id a title is started by.
  ///
  /// @param title_id A title id.
  /// @return The id trimmed and without a trailing `_00`.
  static auto start_id(std::string_view title_id) -> std::string;
  /// @brief Reads a moment PS wrote.
  ///
  /// @param text The text.
  /// @return The moment, or nothing when it is unreadable or not after the
  ///     epoch.
  static auto played_at(std::string_view text) -> std::optional<Time>;
  /// @brief Whether a game list category names an app rather than a game.
  ///
  /// @param category The category.
  /// @return True when it ends in `_app`, ignoring case and surrounding
  ///     whitespace.
  static auto is_app(std::string_view category) -> bool;
  /// @brief Joins the installed titles with the game list into shelves.
  ///
  /// An installed title the game list does not name, or names as an app, is
  /// left out. A title's last use is the console's own when known, otherwise
  /// the account's.
  ///
  /// @param consoles The listed consoles.
  /// @param played The game list, by title key.
  /// @param recent The keys on the game list's first page.
  /// @return One shelf per console, in the same order.
  static auto shelved(const std::vector<ListedConsole>& consoles,
                      const PlayedTitles& played, const TitleKeys& recent)
      -> std::vector<Shelf>;

 private:
  /// @brief What the game list pages gave.
  struct PlayedList {
    /// @brief Every title read, by key; the first row for a key wins.
    PlayedTitles titles;
    /// @brief The keys on the first page, the most recently played.
    TitleKeys recent;
  };

  /// @brief Reads the console directory and enough of the game list, and
  /// builds an answer.
  ///
  /// @param user The user.
  /// @param stop Cancels the requests.
  /// @return The answer; its counts are logged.
  /// @retval users::Unanswered Any failure of the requests.
  [[nodiscard]] auto fetched(users::PsUser& user,
                             const platform::StopToken& stop)
      -> Expected<Held, users::Unanswered>;
  /// @brief Reads the console directory page by page.
  ///
  /// Stops at an empty or short page, a page that adds no new console, or
  /// after installed_page_limit pages (logged). Consoles without a duid, or
  /// with one already seen, are skipped.
  ///
  /// @param http The client the requests are sent through.
  /// @param calls The user's authorised calls.
  /// @return The listed consoles.
  /// @retval users::Unanswered Any failure of a page.
  [[nodiscard]] auto installed_list(net::http::Client& http,
                                    users::ConsoleCalls& calls)
      -> Expected<std::vector<ListedConsole>, users::Unanswered>;
  /// @brief Reads the game list page by page until every installed title is
  /// named.
  ///
  /// Stops once every installed key is covered (checked from the second page
  /// on), at a short page or one that adds nothing, or after
  /// played_page_limit pages.
  ///
  /// @param http The client the requests are sent through.
  /// @param calls The user's authorised calls.
  /// @param installed The installed keys to cover.
  /// @return The titles read and the first page's keys.
  /// @retval users::Unanswered Any failure of a page.
  [[nodiscard]] auto played_list(net::http::Client& http,
                                 users::ConsoleCalls& calls,
                                 const TitleKeys& installed)
      -> Expected<PlayedList, users::Unanswered>;
  /// @brief The answer held for an account.
  ///
  /// @param account The account.
  /// @return The answer, or null when none is held.
  auto held(AccountId account) const -> Held;
  /// @brief Drops the answer held for an account.
  ///
  /// @param account The account.
  void forget(AccountId account);

  /// @brief Turns a console from the directory into a listed console.
  ///
  /// @param registered The console PS listed; moved from.
  /// @return The console with its titles (empty and repeated keys dropped),
  ///     or nothing when its duid is blank.
  static auto listed(psn::rest::RegisteredConsole registered)
      -> std::optional<ListedConsole>;
  /// @brief Whether the game list read so far names every installed title.
  ///
  /// @param installed The installed keys.
  /// @param played The titles read.
  /// @return True when every key is present.
  static auto covered(const TitleKeys& installed, const PlayedTitles& played)
      -> bool;
  /// @brief The shelf order: recently played first, most recent first among
  /// them; then by name ignoring case, then by exact name, then by title id.
  ///
  /// @param left One title.
  /// @param right The other.
  /// @return True when `left` goes before `right`.
  static auto before(const InstalledTitle& left,
                     const InstalledTitle& right) noexcept -> bool;
  /// @brief When a title was last used, by whichever party knew.
  ///
  /// @param title The title.
  /// @return The moment, or nothing when unknown.
  static auto last_used(const InstalledTitle& title) noexcept
      -> std::optional<std::chrono::system_clock::time_point>;
  /// @brief Keeps a concept id only when it is digits within
  /// InstalledTitle::concept_limit.
  ///
  /// @param text PS's concept id.
  /// @return The trimmed digits, or empty text.
  static auto concept_of(std::string_view text) -> std::string;
  /// @brief The answer's consoles as AccountLink compares them.
  ///
  /// @param answer The answer.
  /// @return Their names and device uids, in shelf order.
  static auto link_candidates(const Answer& answer)
      -> std::vector<AccountLink::Listed>;

  /// @brief The registry whose consoles are linked.
  KnownConsoles& known_;
  /// @brief The HTTP provider.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Where requests and counts are logged; null when nothing is
  /// logged.
  platform::DiagnosticsProvider* log_;
  /// @brief Largest reply accepted per page, in bytes.
  std::size_t maximum_response_bytes_;
  /// @brief Shares one read among concurrent askers for the same account.
  concurrency::SharedFlight<Held, users::Unanswered> flights_;
  /// @brief Guards #held_.
  mutable platform::Mutex mutex_;
  /// @brief The latest answer per account id.
  std::map<std::uint64_t, Held> held_;
};

}  // namespace libchiaking::consoles
