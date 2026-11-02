/// @file
/// @brief Declares TrophyRecords, the trophies kept in the platform's
/// record store between runs, one set of records per account.
#pragma once

#include <libchiaking/detail/trophies/ids.hpp>
#include <libchiaking/detail/trophies/title_sets.hpp>
#include <libchiaking/detail/trophies/title_trophies.hpp>
#include <libchiaking/detail/trophies/trophy_summary.hpp>
#include <libchiaking/detail/trophies/trophy_title.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/detail/users/np_title_id.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::trophies::cache {

/// @brief Reads and writes an account's kept trophy answers.
///
/// Records live under `trophies/<account>/` as `summary`, `titles`,
/// `title-<set id>` and `played-<title id>`, each a JSON envelope naming its
/// kind, schema, owner and when it was written. A record that cannot be read,
/// or names another kind, schema or account, is erased and reads as absent.
/// Storage failures are not reported: a failed write just leaves nothing kept.
/// Holds no state of its own beyond its references, so it is as thread-safe as
/// the store.
class TrophyRecords {
 public:
  /// @brief A kept answer with when it was written.
  ///
  /// @tparam Value The answer.
  template <class Value>
  struct Kept {
    /// @brief The answer.
    Value value;
    /// @brief When it was written, on the wall clock.
    Time written_at;
  };

  /// @brief The top segment of every record name.
  static constexpr std::string_view shelf = "trophies";
  /// @brief The schema every record is written with; others are discarded.
  static constexpr std::int64_t schema = 1;

  /// @brief Opens the records.
  ///
  /// @param store Where the records are kept. Must outlive this object.
  /// @param wall_clock Device time records are stamped with. Must outlive
  ///     this object.
  TrophyRecords(platform::RecordStore& store,
                const platform::ClockProvider& wall_clock) noexcept;

  /// @brief Reads the wall clock.
  ///
  /// @return The current time, in milliseconds.
  auto now() const noexcept -> Time;

  /// @brief Reads the kept summary.
  ///
  /// @param account The account.
  /// @return The summary, or nothing when none is kept.
  [[nodiscard]] auto summary(AccountId account)
      -> std::optional<Kept<TrophySummary>>;
  /// @brief Reads the kept trophy titles.
  ///
  /// @param account The account.
  /// @return The titles, or nothing when none are kept.
  [[nodiscard]] auto titles(AccountId account)
      -> std::optional<Kept<std::vector<TrophyTitle>>>;
  /// @brief Reads one title's kept trophies.
  ///
  /// @param account The account.
  /// @param title The title's set.
  /// @return The trophies, or nothing when none are kept for that set.
  [[nodiscard]] auto trophies(AccountId account, const NpCommunicationId& title)
      -> std::optional<Kept<TitleTrophies>>;
  /// @brief Reads the kept trophy sets of one console release.
  ///
  /// @param account The account.
  /// @param title The release.
  /// @return The sets, or nothing when none are kept for that release.
  [[nodiscard]] auto title_sets(AccountId account, const NpTitleId& title)
      -> std::optional<Kept<TitleSets>>;

  /// @brief Keeps the summary, stamped now.
  ///
  /// @param account The account.
  /// @param summary The summary.
  void keep(AccountId account, const TrophySummary& summary);
  /// @brief Keeps the trophy titles, stamped now.
  ///
  /// @param account The account.
  /// @param titles The titles.
  void keep(AccountId account, const std::vector<TrophyTitle>& titles);
  /// @brief Keeps one title's trophies, stamped now.
  ///
  /// @param account The account.
  /// @param trophies The trophies.
  void keep(AccountId account, const TitleTrophies& trophies);
  /// @brief Keeps one console release's trophy sets, stamped now.
  ///
  /// @param account The account.
  /// @param sets The release and its sets.
  void keep(AccountId account, const TitleSets& sets);
  /// @brief Erases every record of the account.
  ///
  /// @param account The account.
  void forget(AccountId account);

 private:
  /// @brief The stored shapes and their conversion from and to values.
  struct Wire;

  /// @brief Names one of an account's records.
  ///
  /// @param account The account.
  /// @param leaf The record's last segment.
  /// @return The name, or nothing when the leaf is not a valid segment.
  static auto name_of(AccountId account, std::string_view leaf)
      -> std::optional<platform::RecordName>;
  /// @brief The leaf of a title's record.
  ///
  /// @param title The title's set.
  /// @return `title-` and the set id in lower case.
  static auto title_leaf(const NpCommunicationId& title) -> std::string;
  /// @brief The leaf of a release's trophy sets record.
  ///
  /// @param title The release.
  /// @return `played-` and the title id in lower case.
  static auto played_leaf(const NpTitleId& title) -> std::string;
  /// @brief Puts an id in lower case after a prefix, as record leaves are
  /// spelled.
  ///
  /// @param prefix The leaf's start.
  /// @param id The id.
  /// @return The leaf.
  static auto lowered_leaf(std::string_view prefix, std::string_view id)
      -> std::string;
  /// @brief Reads and checks an envelope.
  ///
  /// @tparam Body The envelope's body.
  /// @param account The owner it must name.
  /// @param kind The kind it must name.
  /// @param leaf The record's last segment.
  /// @return The body and its stamp, or nothing when absent or unusable.
  template <class Body>
  [[nodiscard]] auto read(AccountId account, std::string_view kind,
                          std::string_view leaf) -> std::optional<Kept<Body>>;
  /// @brief Writes an envelope, stamped now.
  ///
  /// @tparam Body The envelope's body.
  /// @param account The owner.
  /// @param kind The kind.
  /// @param leaf The record's last segment.
  /// @param body The body.
  template <class Body>
  void write(AccountId account, std::string_view kind, std::string_view leaf,
             const Body& body);

  /// @brief Where the records are kept.
  platform::RecordStore& store_;
  /// @brief Device time records are stamped with.
  const platform::ClockProvider& wall_clock_;
};

}  // namespace libchiaking::trophies::cache
