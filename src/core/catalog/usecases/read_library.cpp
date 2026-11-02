#include "src/core/catalog/usecases/read_library.hpp"

#include "src/core/catalog/adapters/ps/ownership_records.hpp"
#include "src/core/catalog/adapters/ps/ps5_library.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/usecases/asker.hpp"

#include <libchiaking/platform/diagnostics/provider.hpp>

#include <string>
#include <utility>
#include <vector>

namespace libchiaking::catalog::usecases {

ReadLibrary::ReadLibrary(platform::HttpProvider& http,
                         const platform::ClockProvider& clock,
                         const platform::ClockProvider& wall_clock,
                         platform::DiagnosticsProvider* log,
                         HeldAnswers& held) noexcept
    : http_(http), clock_(clock), wall_(wall_clock), log_(log), held_(held) {}

auto ReadLibrary::library(users::PsUser& user, const Stop& stop,
                          const FetchProgress& progress)
    -> Expected<LibraryPtr, Unanswered> {
  if (auto refused = Asker::turned_away(user)) return unexpected(*refused);
  const auto account = user.account_id();
  return HeldAnswers::answer(
      held_.in_flight().libraries, HeldAnswers::Question::by(account), stop,
      [&] { return whole_fresh(account); },
      [&]() -> Expected<LibraryPtr, Unanswered> {
        net::http::Client client{http_, clock_, stop, log_};
        const auto held = held_.fresh_library(account);
        if (held) progress.doing(FetchActivity::reusing_held_answer);
        return kept_whole(
            user, client,
            held ? Expected<Library, Unanswered>{*held}
                 : legacy_cloud_stream_part(user, client, progress),
            progress);
      });
}

auto ReadLibrary::afresh(users::PsUser& user, const Stop& stop)
    -> Expected<LibraryPtr, Unanswered> {
  net::http::Client client{http_, clock_, stop, log_};
  const FetchProgress unreported;
  return kept_whole(user, client,
                    legacy_cloud_stream_part(user, client, unreported),
                    unreported);
}

auto ReadLibrary::kept_whole(users::PsUser& user, net::http::Client& client,
                             Expected<Library, Unanswered> legacy_cloud_stream,
                             const FetchProgress& progress)
    -> Expected<LibraryPtr, Unanswered> {
  if (!legacy_cloud_stream) {
    if (!Asker::ends_the_load(legacy_cloud_stream.error())) {
      progress.doing(FetchActivity::scanning_ps5_library);
      scan_ps5_alone(user, client, progress.pages());
    }
    return unexpected(legacy_cloud_stream.error());
  }
  progress.doing(FetchActivity::scanning_ps5_library);
  auto whole = with_ps5_part(
      user, client, std::move(legacy_cloud_stream).value(), progress.pages());
  if (!whole) return unexpected(whole.error());
  return held_.kept(
      std::make_shared<const Library>(std::move(whole.value().library)),
      std::make_shared<const OwnedPs5Games>(std::move(whole.value().ps5)));
}

auto ReadLibrary::whole_fresh(AccountId account) -> LibraryPtr {
  auto held = held_.fresh_library(account);
  return held && held->ps5_ownership() == LibraryState::fetched ? held
                                                                : nullptr;
}

auto ReadLibrary::legacy_cloud_stream_part(users::PsUser& user,
                                           net::http::Client& client,
                                           const FetchProgress& progress)
    -> Expected<Library, Unanswered> {
  progress.doing(FetchActivity::reading_entitlements);
  auto owned = ps::OwnershipRecords{user, client, wall_}.all(progress.pages());
  if (!owned) return unexpected(owned.error());
  auto records = std::move(owned.value().records);
  const auto ignored = std::erase_if(records, [](const OwnedRecord& record) {
    return record.service() != ServiceGeneration::old_service;
  });
  if (ignored != 0)
    client.log(platform::LogSeverity::info,
               "[OWNED] " + std::to_string(ignored) +
                   " PS5 records from the commerce library are ignored; PS5 "
                   "ownership is "
                   "read from the PS5 library");
  auto titles = OwnedRecord::library_of(user.account_id(), records,
                                        owned.value().fetched_at);
  if (!ps::OwnershipRecords::lacks_a_concept(records)) return titles;
  progress.doing(FetchActivity::reading_streamed_ps4_concepts);
  const auto learned =
      ps::OwnershipRecords{user, client, wall_}.concepts_of_streamed_ps4_copies(
          progress.pages());
  if (learned) return titles.with_learned_concepts(learned.value());
  client.log(platform::LogSeverity::warning,
             "[OWNED] the concepts of streamed PS4 copies could not be "
             "read; "
             "the library is kept without them");
  return titles;
}

void ReadLibrary::scan_ps5_alone(users::PsUser& user, net::http::Client& client,
                                 PageCount pages) {
  auto scanned = ps::Ps5Library{user, client, wall_}.scan(std::move(pages));
  if (!scanned) {
    client.log(platform::LogSeverity::warning,
               "[OWNED] the PS5 library could not be read either; PS5 "
               "ownership is unknown");
    return;
  }
  client.log(
      platform::LogSeverity::info,
      "[OWNED] the commerce library could not be read; PS5 ownership is held "
      "from the PS5 library alone");
  auto& read = scanned.value();
  held_.keep_ps5_alone(
      user.account_id(),
      std::make_shared<const OwnedPs5Games>(
          std::move(read.records), read.ownership,
          OwnedPs5Games::Scan{read.read, read.revision_restarts},
          read.fetched_at));
}

auto ReadLibrary::with_ps5_part(users::PsUser& user, net::http::Client& client,
                                Library legacy_cloud_stream, PageCount pages)
    -> Expected<Whole, Unanswered> {
  auto scanned = ps::Ps5Library{user, client, wall_}.scan(std::move(pages));
  if (!scanned && scanned.error().is_cancelled())
    return unexpected(scanned.error());
  if (!scanned) {
    client.log(
        platform::LogSeverity::warning,
        "[OWNED] the PS5 library could not be read; PS5 ownership is unknown");
    return Whole{
        legacy_cloud_stream.with_ps5_part({}, LibraryState::unavailable),
        OwnedPs5Games::unread(held_.now())};
  }
  auto& read = scanned.value();
  OwnedPs5Games games{std::move(read.records),
                      read.ownership,
                      {read.read, read.revision_restarts},
                      read.fetched_at};
  auto library =
      legacy_cloud_stream.with_ps5_part(games.games(), read.ownership);
  return Whole{std::move(library), std::move(games)};
}

auto ReadLibrary::owned_by(users::PsUser& user, const Stop& stop,
                           const FetchProgress& progress)
    -> Expected<HeldAnswers::Owned, Unanswered> {
  auto fetched = library(user, stop, progress);
  if (!fetched && Asker::ends_the_load(fetched.error()))
    return unexpected(fetched.error());
  return held_.owned_after(user.account_id(),
                           fetched ? std::move(fetched).value() : nullptr);
}

}  // namespace libchiaking::catalog::usecases
