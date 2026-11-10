/// @file
/// @brief Declares usecases::ShowImages, the use case that serves game artwork
/// from the image shelf or the store.
#pragma once

#include "libchiaking/detail/catalog/catalog.hpp"
#include "src/core/catalog/adapters/cache/held_answers.hpp"
#include "src/core/utils/concurrency/turnstile.hpp"

#include <cstddef>
#include <vector>

namespace libchiaking::catalog::usecases {

/// @brief Serves game images, from the image shelf or a download.
///
/// Concurrent callers asking the same image share one request. Downloads are
/// limited to downloads_at_once at a time with at most downloads_in_queue
/// waiting. Borrows the provider, clock, log and held answers, which must
/// outlive it. Safe to call from several threads. Not copyable.
class ShowImages {
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
  /// @param log Receives log lines; may be null.
  /// @param held Where images are held. Must outlive this object.
  ShowImages(platform::HttpProvider& http,
             platform::ThreadingProvider& threading,
             const platform::ClockProvider& clock,
             platform::DiagnosticsProvider* log, HeldAnswers& held);
  /// @brief Not copyable: it owns the download turnstile.
  ShowImages(const ShowImages&) = delete;
  /// @brief Not copyable: it owns the download turnstile.
  auto operator=(const ShowImages&) -> ShowImages& = delete;

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
  /// @brief Downloads an image once a download turn is free.
  ///
  /// @param image The image.
  /// @param stop Abandons the call.
  /// @return The bytes; a cancellation; Unanswered::Reason::unavailable when
  ///     the queue is full; the download's failure.
  [[nodiscard]] auto download(const ImageRef& image, const Stop& stop)
      -> Expected<std::vector<std::byte>, Unanswered>;

  /// @brief Sends requests; not owned.
  platform::HttpProvider& http_;
  /// @brief The monotonic clock request deadlines are measured on; not
  /// owned.
  const platform::ClockProvider& clock_;
  /// @brief Receives log lines; may be null.
  platform::DiagnosticsProvider* log_;
  /// @brief The held answers; not owned.
  HeldAnswers& held_;
  /// @brief Limits concurrent image downloads.
  concurrency::Turnstile downloads_;
};

}  // namespace libchiaking::catalog::usecases
