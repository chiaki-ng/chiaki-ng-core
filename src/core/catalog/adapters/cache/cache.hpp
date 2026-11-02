/// @file
/// @brief Declares cache::Cache, the catalog's typed access to the
/// platform::RecordStore, and the Envelope every record is stored in.
///
/// Each record kind (streamable lists, categories, libraries, refusals, game
/// details, the exclusion list, images) supplies its name, schema, owner and
/// body; Cache wraps the body in an Envelope, writes it atomically, and on
/// reading discards anything whose kind, schema, owner or age does not
/// match, so a stale or foreign record is never served.
#pragma once

#include "src/core/catalog/adapters/cache/record_format.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/utils/json/ps_fields.hpp"

#include <libchiaking/detail/catalog/store_locale.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/account_id.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>
#include <libchiaking/platform/storage/record_store.hpp>

#include <array>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace libchiaking::catalog::cache {

/// @brief The JSON object one record is stored as. Plain data.
///
/// @tparam Body The record kind's stored body; must have a glz::meta.
template <class Body>
struct Envelope {
  /// @brief The record kind, such as the record type's kind constant.
  std::string record;
  /// @brief The kind's schema version; a record of another version is
  /// discarded.
  std::optional<std::int64_t> schema;
  /// @brief When the record was written, in wall-clock milliseconds since
  /// the Unix epoch.
  std::int64_t written_at_ms{};
  /// @brief The account the record belongs to, in decimal; absent for
  /// shared records.
  std::optional<std::string> account;
  /// @brief The store locale the record belongs to; absent for per-account
  /// and global records.
  std::optional<std::string> region;
  /// @brief The kind's own content.
  Body body{};
};

/// @brief The key of a record that belongs to no account and no region,
/// such as the exclusion list.
struct Everyone {
  /// @brief All Everyone keys are equal.
  friend auto operator==(const Everyone&, const Everyone&) -> bool = default;
};

/// @brief The few body fields cache inspection reads from any record, all
/// read leniently. Plain data.
struct Summary {
  /// @brief The product a game details record is for, when it names one.
  std::optional<std::string> product;
  /// @brief The concept a game details record is for, when it names one.
  std::optional<std::string> concept_id;
  /// @brief When the body's answer was fetched, in wall-clock milliseconds,
  /// for kinds that store it.
  std::optional<std::int64_t> fetched_at_ms;
};

/// @brief Typed reads and writes of catalog records in the host's record
/// store.
///
/// Holds references to the store and the wall clock, which must outlive it.
/// Every call may block on the store's disk I/O. Cache has no lock of its
/// own: HeldAnswers serialises its reads and writes under its disk lock,
/// and other users must do the same or rely on the store being safe for
/// concurrent use.
class Cache {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = cache::Time;

  /// @brief The largest record read back, in bytes; a larger one is
  /// treated as absent.
  static constexpr std::size_t maximum_bytes = 64U * 1024U * 1024U;
  /// @brief The first name segment of per-account records:
  /// user/{account}/{kind}.
  static constexpr std::string_view users_segment = "user";
  /// @brief Single-file caches written by earlier versions; erased when a
  /// Cache is built.
  static constexpr std::array<std::string_view, 17> old_file_names{
      "unified_catalog_v4.json",        "ps5_cloud_catalog_v8.json",
      "ps5_cloud_library_v2.json",      "ps_plus_store_tags_v2.json",
      "ps_plus_store_tags_failure.txt", "ps5_cloud_catalog_v7.json",
      "unified_catalog_v3.json",        "ps5_cloud_catalog_v6.json",
      "ps5_cloud_library.json",         "psnow_catalog.json",
      "unified_catalog_v2.json",        "unified_catalog_v1.json",
      "ps5_cloud_catalog_v5.json",      "ps5_cloud_catalog_v4.json",
      "ps5_cloud_catalog_v3.json",      "ps5_cloud_catalog_v2.json",
      "ps5_cloud_catalog.json",
  };
  /// @brief Records written by earlier versions under names no longer read;
  /// erased when a Cache is built.
  static constexpr std::array<std::string_view, 1> old_record_names{
      "exclusions/portal",
  };

  /// @brief One record as read back, before its kind checks its body.
  ///
  /// @tparam Body The stored body type.
  template <class Body>
  struct Kept {
    /// @brief The record kind written in the envelope.
    std::string kind;
    /// @brief The schema version written in the envelope.
    std::optional<std::int64_t> schema;
    /// @brief When the record was written; wall-clock milliseconds.
    Time written_at;
    /// @brief The owning account in decimal, when per-account.
    std::optional<std::string> account;
    /// @brief The owning store locale, when per-region.
    std::optional<std::string> region;
    /// @brief The stored body.
    Body body;
  };

  /// @brief Opens the cache and erases the files of earlier versions.
  ///
  /// Blocks on the store while those files are erased; failures are
  /// ignored.
  ///
  /// @param store Where records live. Must outlive the cache.
  /// @param wall_clock Stamps writes and ages reads. Must outlive the cache.
  Cache(platform::RecordStore& store,
        const platform::ClockProvider& wall_clock);

  /// @brief The wall clock now.
  ///
  /// @return Wall-clock time floored to milliseconds.
  auto now() const noexcept -> Time;

  /// @brief The name of one account's record of a kind.
  ///
  /// @param account The account.
  /// @param leaf The record kind, the last segment.
  /// @return user/{account}/{leaf}, or std::nullopt when the leaf is not a
  ///     valid name segment.
  [[nodiscard]] static auto user_record(AccountId account,
                                        std::string_view leaf)
      -> std::optional<platform::RecordName>;
  /// @brief The name of a shared record.
  ///
  /// @param kind The record kind, the first segment.
  /// @param scope The scope inside the kind, such as a lower-case locale.
  /// @return {kind}/{scope}, or std::nullopt when either is not a valid
  ///     name segment.
  [[nodiscard]] static auto shared_record(std::string_view kind,
                                          std::string_view scope)
      -> std::optional<platform::RecordName>;

  /// @brief Reads the record of a kind for a key.
  ///
  /// A stored record of another kind, schema, account or region, one older
  /// than the kind's kept_for (when it has one), or one whose body does not
  /// read back is erased and reported as absent.
  ///
  /// @tparam Record The record kind. Provides Key, Body, kind, schema,
  ///     name_of(Key) and from_body(Body, Key), and optionally kept_for.
  /// @param key Which record of the kind.
  /// @return The record, std::nullopt when there is none (or it was
  ///     discarded, or the key has no valid name), or
  ///     Unanswered::Reason::storage when the store could not be read.
  template <class Record>
  [[nodiscard]] auto read(const typename Record::Key& key)
      -> Expected<std::optional<Record>, Unanswered> {
    const auto name = Record::name_of(key);
    if (!name) return std::optional<Record>{};
    auto held = kept_as<typename Record::Body>(*name);
    if (!held) return unexpected(held.error());
    const auto owner = owner_of(key);
    auto& envelope = held.value();
    if (!envelope || envelope->kind != Record::kind ||
        envelope->schema != Record::schema ||
        envelope->account != owner.account ||
        envelope->region != owner.region) {
      discard(*name);
      return std::optional<Record>{};
    }
    auto record = still_kept<Record>(now() - envelope->written_at)
                      ? Record::from_body(envelope->body, key)
                      : std::optional<Record>{};
    if (!record) discard(*name);
    return record;
  }

  /// @brief Writes a record atomically, replacing any earlier one.
  ///
  /// @tparam Record The record kind. Provides key(), body(), kind, schema
  ///     and name_of(Key).
  /// @param record The record.
  /// @return Nothing on success; Unanswered::Reason::storage when the key
  ///     has no valid name, the body cannot be serialised, or the store
  ///     refuses the write.
  template <class Record>
  [[nodiscard]] auto write(const Record& record) -> Expected<void, Unanswered> {
    const auto name = Record::name_of(record.key());
    if (!name) return unexpected(Unanswered{Unanswered::Reason::storage});
    auto owner = owner_of(record.key());
    return written(*name, Envelope<typename Record::Body>{
                              .record = std::string{Record::kind},
                              .schema = Record::schema,
                              .written_at_ms = milliseconds_of(now()),
                              .account = std::move(owner.account),
                              .region = std::move(owner.region),
                              .body = record.body()});
  }

  /// @brief Erases the record of a kind for a key.
  ///
  /// @tparam Record The record kind. Provides Key and name_of(Key).
  /// @param key Which record.
  /// @return Nothing when it was erased, was absent, or the key has no valid
  ///     name; Unanswered::Reason::storage when the store failed.
  template <class Record>
  [[nodiscard]] auto erase(const typename Record::Key& key)
      -> Expected<void, Unanswered> {
    const auto name = Record::name_of(key);
    if (!name) return {};
    return erase_named(*name);
  }

  /// @brief Reads a record's envelope without checking its kind, owner or
  /// age.
  ///
  /// @tparam Body The body type to read; unknown keys are ignored and
  ///     missing keys fail.
  /// @param name The record's name.
  /// @return The envelope, std::nullopt when the record is absent, larger
  ///     than maximum_bytes or does not parse, or
  ///     Unanswered::Reason::storage when the store could not be read.
  template <class Body>
  [[nodiscard]] auto kept_as(const platform::RecordName& name)
      -> Expected<std::optional<Kept<Body>>, Unanswered> {
    auto stored = read_blob(name);
    if (!stored) return unexpected(stored.error());
    if (!stored.value() || stored.value()->size() > maximum_bytes)
      return std::optional<Kept<Body>>{};
    Envelope<Body> envelope{};
    const auto& bytes = *stored.value();
    if (json::read_into<record_reading>(
            envelope,
            std::string_view{reinterpret_cast<const char*>(bytes.data()),
                             bytes.size()}))
      return std::optional<Kept<Body>>{};
    return std::optional<Kept<Body>>{
        Kept<Body>{std::move(envelope.record), envelope.schema,
                   time_of(envelope.written_at_ms), std::move(envelope.account),
                   std::move(envelope.region), std::move(envelope.body)}};
  }

  /// @brief Erases every record of one account.
  ///
  /// @param account The account.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     store failed.
  [[nodiscard]] auto forget(AccountId account) -> Expected<void, Unanswered>;

  /// @brief Lists the records under a name prefix.
  ///
  /// @param shelf The first name segment, such as a record kind.
  /// @return The records with their sizes and times; possibly empty;
  ///     Unanswered::Reason::storage when the prefix is not a valid name or
  ///     the listing failed.
  [[nodiscard]] auto listed(std::string_view shelf)
      -> Expected<std::vector<platform::RecordListing>, Unanswered>;
  /// @brief Reads the envelope and summary fields of any record.
  ///
  /// @param name The record's name.
  /// @return As kept_as with Summary as the body.
  [[nodiscard]] auto kept(const platform::RecordName& name)
      -> Expected<std::optional<Kept<Summary>>, Unanswered>;
  /// @brief Erases every record under a name prefix.
  ///
  /// @param shelf The first name segment.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     prefix is not valid or the store failed.
  [[nodiscard]] auto erase_shelf(std::string_view shelf)
      -> Expected<void, Unanswered>;
  /// @brief Erases one kind of record for every account.
  ///
  /// Lists every record under the users segment and erases each whose last
  /// segment is the kind. Not atomic: a record written for an account while
  /// this runs may survive or be erased. Blocks on the store.
  ///
  /// @param leaf The record kind, the last name segment, such as "refusals".
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     listing failed or, after trying every record, the last erase that
  ///     failed.
  [[nodiscard]] auto erase_for_every_account(std::string_view leaf)
      -> Expected<void, Unanswered>;
  /// @brief Erases one record by name.
  ///
  /// @param name The record's name.
  /// @return Nothing when erased or already absent;
  ///     Unanswered::Reason::storage for any other store failure.
  [[nodiscard]] auto erase_named(const platform::RecordName& name)
      -> Expected<void, Unanswered>;
  /// @brief Reads a record's raw bytes.
  ///
  /// @param name The record's name.
  /// @return The bytes, std::nullopt when absent, or
  ///     Unanswered::Reason::storage when the store failed.
  [[nodiscard]] auto read_blob(const platform::RecordName& name)
      -> Expected<std::optional<std::vector<std::byte>>, Unanswered>;
  /// @brief Writes a record's raw bytes atomically.
  ///
  /// @param name The record's name.
  /// @param bytes The contents.
  /// @return Nothing on success; Unanswered::Reason::storage when the
  ///     store refused.
  [[nodiscard]] auto write_blob(const platform::RecordName& name,
                                std::span<const std::byte> bytes)
      -> Expected<void, Unanswered>;

 private:
  /// @brief Who a record belongs to, as written in its envelope.
  struct Owner {
    /// @brief The account in decimal, for per-account records.
    std::optional<std::string> account;
    /// @brief The store locale text, for per-region records.
    std::optional<std::string> region;
  };

  /// @brief Whether a record of this age may still be served.
  ///
  /// @tparam Record The record kind; its optional kept_for bounds the age.
  /// @param age How long ago the record was written.
  /// @return True when the kind has no kept_for, or the age is from zero
  ///     to kept_for inclusive; false for a record from the future.
  template <class Record>
  static constexpr auto still_kept(std::chrono::milliseconds age) noexcept
      -> bool {
    if constexpr (requires { Record::kept_for; })
      return age >= std::chrono::milliseconds::zero() &&
             age <= Record::kept_for;
    else
      return true;
  }

  /// @brief Serialises an envelope and writes it.
  ///
  /// @tparam Body The body type.
  /// @param name The record's name.
  /// @param envelope The envelope.
  /// @return Nothing on success; Unanswered::Reason::storage when it cannot
  ///     be serialised or stored.
  template <class Body>
  [[nodiscard]] auto written(const platform::RecordName& name,
                             const Envelope<Body>& envelope)
      -> Expected<void, Unanswered> {
    std::string text;
    if (glz::write<glz::opts{}>(envelope, text))
      return unexpected(Unanswered{Unanswered::Reason::storage});
    return write_blob(name, std::as_bytes(std::span{text.data(), text.size()}));
  }

  /// @brief The owner of a per-account record.
  ///
  /// @param account The account.
  /// @return The account in decimal and no region.
  static auto owner_of(AccountId account) -> Owner;
  /// @brief The owner of a per-region record.
  ///
  /// @param locale The store locale.
  /// @return The locale text and no account.
  static auto owner_of(const StoreLocale& locale) -> Owner;
  /// @brief The owner of a global record.
  ///
  /// @param everyone The global key.
  /// @return Neither account nor region.
  static auto owner_of(const Everyone& everyone) -> Owner;
  /// @brief The owner of a record keyed by a struct with a locale.
  ///
  /// @tparam Key A key with a locale member, such as a game details key.
  /// @param key The key.
  /// @return The key's locale text and no account.
  template <class Key>
    requires requires(const Key& key) {
      { key.locale } -> std::convertible_to<const StoreLocale&>;
    }
  static auto owner_of(const Key& key) -> Owner {
    return owner_of(key.locale);
  }

  /// @brief Erases a record that may not be served; failures are ignored.
  ///
  /// @param name The record's name.
  void discard(const platform::RecordName& name);
  /// @brief Erases each of old_file_names and old_record_names; failures are
  /// ignored.
  void erase_leftovers();

  /// @brief Where records live; not owned.
  platform::RecordStore& store_;
  /// @brief Stamps writes and ages reads; not owned.
  const platform::ClockProvider& wall_clock_;
};

}  // namespace libchiaking::catalog::cache

/// @brief How an Envelope is spelled in JSON.
///
/// @tparam Body The record kind's body.
template <class Body>
struct glz::meta<libchiaking::catalog::cache::Envelope<Body>> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::Envelope<Body>;
  /// @brief The keys record, schema, writtenAtMs, account, region and body.
  static constexpr auto value =
      glz::object("record", &T::record, "schema", &T::schema, "writtenAtMs",
                  &T::written_at_ms, "account", &T::account, "region",
                  &T::region, "body", &T::body);
};

/// @brief How Summary reads a body: product, concept and fetchedAtMs, each
/// leniently so a body of any kind can be summarised.
template <>
struct glz::meta<libchiaking::catalog::cache::Summary> {
  /// @brief The type described.
  using T = libchiaking::catalog::cache::Summary;
  /// @brief The keys product, concept and fetchedAtMs.
  static constexpr auto value = glz::object(
      "product", libchiaking::json::ps::lenient(&T::product), "concept",
      libchiaking::json::ps::lenient(&T::concept_id), "fetchedAtMs",
      libchiaking::json::ps::lenient(&T::fetched_at_ms));
};
