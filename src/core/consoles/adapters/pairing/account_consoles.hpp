/// @file
/// @brief Declares AccountConsoles, which lists the consoles a PSN account has
/// registered for Remote Play, so a console can be paired from the account
/// rather than by its PIN.
///
/// PS5 consoles come from PS's console directory (cloud assisted
/// navigation), asked with the user's Remote Play token. PSN keeps no list of
/// PS4 consoles, so a PS4 account is answered with the single placeholder
/// AccountConsole::main_ps4().
#pragma once

#include "libchiaking/detail/consoles/account_console.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/expected.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/threading.hpp"
#include "src/core/net/transport/http/endpoint.hpp"
#include "src/core/psn/rest/cloud_assisted_navigation/cloud_assisted_navigation.hpp"

#include <cstddef>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
class HttpProvider;
}  // namespace libchiaking::platform

namespace libchiaking::net::http {
class Client;
}  // namespace libchiaking::net::http

namespace libchiaking::users {
class ConsoleCalls;
}  // namespace libchiaking::users

namespace libchiaking::consoles {

/// @brief The consoles one PSN account has registered, as the consoles
/// context names them.
///
/// The object holds only the providers it was made with and its reply limit;
/// it keeps no state between calls and may be used from several threads at
/// once. The providers must outlive it.
class AccountConsoles {
 public:
  /// @brief How many consoles PS's console directory returns per page.
  static constexpr std::size_t page_size = psn::rest::list_clients_page_size;
  /// @brief Most pages of the console directory read in one listing.
  static constexpr int page_limit = 50;

  /// @brief The account's consoles, and how many listed entries could not be
  /// used.
  struct Listed {
    /// @brief The consoles that carry a valid device uid, in PS's order.
    std::vector<AccountConsole> consoles;
    /// @brief How many listed entries were dropped: those PS sent without
    /// a readable device uid or name, plus those AccountConsole::create
    /// refused.
    std::size_t skipped{};
  };

  /// @brief Makes a lister that sends its requests through `http`.
  ///
  /// @param http The HTTP provider; must outlive this object.
  /// @param clock The monotonic clock request deadlines are measured on;
  ///     must outlive this object.
  /// @param log Where the requests and the count of listed consoles are
  ///     logged; null logs nothing. Must outlive this object.
  /// @param maximum_response_bytes Largest reply accepted, in bytes.
  AccountConsoles(platform::HttpProvider& http,
                  const platform::ClockProvider& clock,
                  platform::DiagnosticsProvider* log = nullptr,
                  std::size_t maximum_response_bytes =
                      net::http::default_maximum_response_bytes) noexcept;

  /// @brief Lists the consoles `user`'s account has registered for one
  /// console generation.
  ///
  /// For Generation::ps4 nothing is sent and the answer is the main PS4
  /// placeholder alone. For Generation::ps5 the console directory is read
  /// page by page with the user's Remote Play token
  /// (users::ConsoleCalls::account_consoles) until a page holds fewer than
  /// page_size entries, or page_limit pages have been read (logged as a
  /// warning). Blocks the calling thread for the requests, each bounded by
  /// the HTTP client's timeouts and `stop`.
  ///
  /// @param user The user whose account is asked.
  /// @param generation Which consoles to list.
  /// @param stop Cancels the request.
  /// @return The listed consoles; the count of usable ones is logged.
  /// @retval users::Unanswered::Reason::signed_out The user is off PSN.
  /// @retval users::Unanswered Any failure of a console directory request,
  ///     including a missing or refused Remote Play token and cancellation;
  ///     the pages already read are discarded.
  [[nodiscard]] auto of(users::PsUser& user, Generation generation,
                        const platform::StopToken& stop)
      -> Expected<Listed, users::Unanswered>;

  /// @brief Turns a page of PS's console directory into account consoles.
  ///
  /// Every entry is taken as a PS5. An entry without a device uid, or whose
  /// device uid or name AccountConsole::create refuses, is counted in
  /// Listed::skipped together with the page's unaddressable entries.
  ///
  /// @param page The page PS returned; its entries are moved from.
  /// @return The usable consoles and the count of dropped ones.
  static auto translated(psn::rest::ClientsPage page) -> Listed;

 private:
  /// @brief Reads the console directory page by page and joins the pages.
  ///
  /// Stops after the first page holding fewer than page_size entries, or
  /// after page_limit pages. Blocks for every request.
  ///
  /// @param http The client the requests are sent through.
  /// @param calls The user's authorised calls the pages are asked with.
  /// @return Every page's usable consoles in PS's order, with the dropped
  ///     entries of all pages counted in Listed::skipped.
  /// @retval users::Unanswered The first page request that failed.
  [[nodiscard]] auto paged(net::http::Client& http, users::ConsoleCalls& calls)
      -> Expected<Listed, users::Unanswered>;

  /// @brief The HTTP provider requests are sent through.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on.
  const platform::ClockProvider& clock_;
  /// @brief Where requests are logged; null when nothing is logged.
  platform::DiagnosticsProvider* log_;
  /// @brief Largest reply accepted from the console directory, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::consoles
