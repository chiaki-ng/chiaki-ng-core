/// @file
/// @brief Declares TitleClearance, the port a cloud start asks whether a title
/// may stream, and CatalogClearance, which answers it from the catalog.
#pragma once

#include "libchiaking/detail/catalog/launch_check.hpp"
#include "libchiaking/detail/catalog/launch_verdict.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/stream_refusal.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "libchiaking/platform/runtime/threading.hpp"

namespace libchiaking::catalog {
class Catalog;
}

namespace libchiaking::play {

/// @brief The question a cloud start asks before it launches a title: may
/// this user stream it now, and what exactly should be launched?
///
/// The start use case depends on this port rather than on the catalog, so
/// tests can clear or refuse titles without one. Implementations are shared by
/// every start and must allow calls from several threads. Not copyable.
class TitleClearance {
 public:
  /// @brief Builds the port.
  TitleClearance() = default;
  /// @brief Not copyable; an implementation stands for one catalog.
  TitleClearance(const TitleClearance&) = delete;
  /// @brief Not copy-assignable; an implementation stands for one catalog.
  auto operator=(const TitleClearance&) -> TitleClearance& = delete;
  /// @brief Releases the implementation.
  virtual ~TitleClearance() = default;

  /// @brief Judges from held answers alone whether the title may launch; makes
  /// no request and does not block.
  ///
  /// @param user The user who wants to play.
  /// @param product The title to launch.
  /// @return The verdict from what is already known.
  virtual auto held(users::PsUser& user,
                    const catalog::ProductId& product) const
      -> catalog::LaunchVerdict = 0;
  /// @brief Makes every check needed before a cloud stream starts.
  ///
  /// Blocks on requests to PS until the checks finish or @p stop fires.
  ///
  /// @param user The user who wants to play.
  /// @param product The title to launch.
  /// @param stop Cancels any request.
  /// @param observer Told as each check starts.
  /// @return An allowed verdict saying what to launch, or the refusal. A check
  ///     that could not complete, cancellation included, is a refusal of kind
  ///     LaunchRefusal::Kind::check_unavailable.
  virtual auto clear(users::PsUser& user, const catalog::ProductId& product,
                     const platform::StopToken& stop,
                     const catalog::LaunchCheckObserver& observer)
      -> catalog::LaunchVerdict = 0;
  /// @brief Records that PS's streaming service refused to start the title,
  /// so later verdicts refuse it too.
  ///
  /// @param user The user who was refused.
  /// @param product The title that was refused.
  /// @param kind What the streaming service said.
  virtual void remember_refusal(users::PsUser& user,
                                const catalog::ProductId& product,
                                catalog::StreamRefusal::Kind kind) = 0;
};

/// @brief Answers TitleClearance from a catalog::Catalog.
///
/// Holds a reference to the catalog, which must outlive it; thread safety is
/// the catalog's.
class CatalogClearance final : public TitleClearance {
 public:
  /// @brief Builds the answerer over one catalog.
  ///
  /// @param catalog The catalog asked; must outlive this object.
  explicit CatalogClearance(catalog::Catalog& catalog) noexcept
      : catalog_(catalog) {}

  /// @brief Answers with catalog::Catalog::can_launch.
  ///
  /// @param user The user who wants to play.
  /// @param product The title to launch.
  /// @return The catalog's verdict from held answers.
  auto held(users::PsUser& user, const catalog::ProductId& product) const
      -> catalog::LaunchVerdict override;
  /// @brief Answers with catalog::Catalog::clear_for_launch; blocks on requests
  /// to PS.
  ///
  /// @param user The user who wants to play.
  /// @param product The title to launch.
  /// @param stop Cancels any request.
  /// @param observer Told as each check starts.
  /// @return The catalog's verdict.
  auto clear(users::PsUser& user, const catalog::ProductId& product,
             const platform::StopToken& stop,
             const catalog::LaunchCheckObserver& observer)
      -> catalog::LaunchVerdict override;
  /// @brief Passes the refusal to catalog::Catalog::remember_refusal.
  ///
  /// @param user The user who was refused.
  /// @param product The title that was refused.
  /// @param kind What the streaming service said.
  void remember_refusal(users::PsUser& user, const catalog::ProductId& product,
                        catalog::StreamRefusal::Kind kind) override;

 private:
  /// @brief The catalog every question goes to.
  catalog::Catalog& catalog_;
};

}  // namespace libchiaking::play
