/// @file
/// @brief Declares usecases::ShowDetails, the use case behind a game's details
/// page: its store details and its images.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/catalog/usecases/list_games.hpp"
#include "src/core/utils/concurrency/turnstile.hpp"

#include <cstddef>
#include <optional>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Serves game details and images, from what is held or from the store.
///
/// Concurrent callers asking the same details or image share one request.
/// Image downloads are limited to downloads_at_once at a time with at most
/// downloads_in_queue waiting. Borrows the provider, clock, log, held answers
/// and ListGames, which must outlive it. Safe to call from several threads.
/// Not copyable.
class ShowDetails {
 public:
  /// @brief The token that abandons a call.
  using Stop = platform::StopToken;

  /// @brief The most image downloads running at once.
  static constexpr std::size_t downloads_at_once = 4;
  /// @brief The most image downloads waiting for a turn; more are refused.
  static constexpr std::size_t downloads_in_queue = 64;

  /// @brief Sets up the use case.
  ///
  /// @param http Sends the requests. Must outlive this object.
  /// @param threading Makes the download turnstile's lock and condition
  ///     variable. Must outlive this object.
  /// @param clock The monotonic clock the turnstile's waits measure on. Must
  ///     outlive this object.
  /// @param wall_clock Stamps the details. Must outlive this object.
  /// @param log Receives log lines; may be null.
  /// @param held Where details and images are held. Must outlive this object.
  /// @param list_games Gives the held games, to find a product's concept. Must
  ///     outlive this object.
  ShowDetails(platform::HttpProvider& http,
              platform::ThreadingProvider& threading,
              const platform::ClockProvider& clock,
              const platform::ClockProvider& wall_clock,
              platform::DiagnosticsProvider* log, HeldAnswers& held,
              ListGames& list_games);
  /// @brief Not copyable: it owns the download turnstile.
  ShowDetails(const ShowDetails&) = delete;
  /// @brief Not copyable: it owns the download turnstile.
  auto operator=(const ShowDetails&) -> ShowDetails& = delete;

  /// @brief The details of a product.
  ///
  /// Asks the products endpoint in the held list's settled locale (else the
  /// account's region, else English US) with the account's age. When the store
  /// does not list the product, retries by the concept the held games, the
  /// held list or the library give it, and returns those details under the
  /// product. Blocks for the requests needed.
  ///
  /// @param user The user.
  /// @param product The product.
  /// @param stop Abandons the call.
  /// @return The details; the sign-in failure;
  ///     Unanswered::Reason::not_listed when the age is unknown or neither the
  ///     product nor its concept is listed; otherwise the request's failure.
  [[nodiscard]] auto details(users::PsUser& user, const ProductId& product,
                             const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  /// @brief The details of a concept.
  ///
  /// Asked in the held list's settled locale, else the account's region, else
  /// English US. The user's sign-in is not checked. Details are held only when
  /// every part was read.
  ///
  /// @param user The user, for the region.
  /// @param grouped_under The concept.
  /// @param stop Abandons the call.
  /// @return The details; the media query's failure; a cancellation.
  [[nodiscard]] auto details(users::PsUser& user,
                             const ConceptId& grouped_under, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  /// @brief An image's bytes, from the image shelf or a download.
  ///
  /// A downloaded image is kept on the shelf; when the shelf cannot store it
  /// the bytes are still returned.
  ///
  /// @param image The image.
  /// @param stop Abandons the call, including the wait for a download turn.
  /// @return The bytes; a cancellation; Unanswered::Reason::unavailable when
  ///     the download queue is full; the download's failure; or
  ///     Unanswered::Reason::rejected when the image is too large to keep.
  [[nodiscard]] auto image(const ImageRef& image, const Stop& stop)
      -> Expected<std::vector<std::byte>, Unanswered>;

 private:
  /// @brief The locale details are asked in.
  ///
  /// @param user The user.
  /// @param seen The held games, when any.
  /// @return The held list's settled locale, else the account's region, else
  ///     English US.
  static auto locale_of(const users::PsUser& user,
                        const std::optional<ListGames::View>& seen)
      -> StoreLocale;
  /// @brief Product details, held or fetched once for every concurrent asker.
  ///
  /// @param locale The locale.
  /// @param age The account's real age.
  /// @param product The product.
  /// @param stop Abandons the call.
  /// @return The details; the request's failure.
  [[nodiscard]] auto sold_as(const StoreLocale& locale, int age,
                             const ProductId& product, const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  /// @brief Concept details, held or fetched once for every concurrent asker.
  ///
  /// @param locale The locale.
  /// @param grouped_under The concept.
  /// @param stop Abandons the call.
  /// @return The details; the request's failure.
  [[nodiscard]] auto grouped_as(const StoreLocale& locale,
                                const ConceptId& grouped_under,
                                const Stop& stop)
      -> Expected<GameDetails, Unanswered>;
  /// @brief Downloads an image once a download turn is free.
  ///
  /// @param image The image.
  /// @param stop Abandons the call.
  /// @return The bytes; a cancellation; Unanswered::Reason::unavailable when
  ///     the queue is full; the download's failure.
  [[nodiscard]] auto download(const ImageRef& image, const Stop& stop)
      -> Expected<std::vector<std::byte>, Unanswered>;
  /// @brief The concept a product belongs to, from what is held.
  ///
  /// Tries the game listed under the product (or holding it), then the list's
  /// entry, then the library's learned concept.
  ///
  /// @param seen The held games.
  /// @param owned The library; may be null.
  /// @param product The product.
  /// @return The concept, or std::nullopt when nothing names one.
  static auto concept_of(const ListGames::View& seen, const Library* owned,
                         const ProductId& product) -> std::optional<ConceptId>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief Starts the threads concept reads run on; not owned.
  platform::ThreadingProvider& threading_;
  /// @brief The monotonic clock request deadlines are measured on; not
  /// owned.
  const platform::ClockProvider& clock_;
  /// @brief The wall clock; not owned.
  const platform::ClockProvider& wall_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
  /// @brief Gives the held games; not owned.
  ListGames& list_games_;
  /// @brief Limits concurrent image downloads.
  concurrency::Turnstile downloads_;
};

}  // namespace libchiaking::catalog::usecases
