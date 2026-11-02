/// @file
/// @brief Declares ps::OwnershipRecords, the adapter that reads the commerce
/// internal_entitlements list LegacyCloudStream ownership is built from.
///
/// This list serves LegacyCloudStream only; its PS5 records are ignored for PS5
/// ownership. It also reads the concepts of
/// streamed PS4 copies, which the main list often leaves out.
#pragma once

#include "src/core/catalog/adapters/ps/store_listing.hpp"
#include "src/core/catalog/adapters/ps/wall_stamp.hpp"
#include "src/core/catalog/model/library/owned_record.hpp"
#include "src/core/catalog/model/page_count.hpp"
#include "src/core/catalog/model/unanswered.hpp"
#include "src/core/net/transport/http/client.hpp"

#include <libchiaking/detail/catalog/library.hpp>
#include <libchiaking/detail/catalog/platform.hpp>
#include <libchiaking/detail/expected.hpp>
#include <libchiaking/detail/users/ps_user.hpp>
#include <libchiaking/platform/runtime/clock_provider.hpp>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace libchiaking::catalog::ps {

/// @brief Reads one user's commerce entitlements.
///
/// Borrows the user, client and clock, which must outlive it. Used on the
/// thread that runs the catalog call.
class OwnershipRecords {
 public:
  /// @brief Wall-clock time in milliseconds since the Unix epoch.
  using Time = WallStamp::Time;

  /// @brief The most distinct entitlements one_of asks about.
  static constexpr std::size_t lookup_limit = 3;
  /// @brief The longest icon URL kept, in bytes; longer ones are cut.
  static constexpr std::size_t icon_url_limit = 1023;

  /// @brief Every record of one read.
  struct Listed {
    /// @brief The records that parsed, in PS's order.
    std::vector<OwnedRecord> records;
    /// @brief When the read finished.
    Time fetched_at{};
  };

  /// @brief Sets up the reads.
  ///
  /// @param user The signed-in user. Must outlive this object.
  /// @param http Sends the requests. Must outlive this object.
  /// @param wall Stamps the read. Must outlive this object.
  /// @param maximum_response_bytes The largest reply accepted per page.
  OwnershipRecords(users::PsUser& user, net::http::Client& http,
                   const platform::ClockProvider& wall,
                   std::size_t maximum_response_bytes =
                       net::http::default_maximum_response_bytes) noexcept;

  /// @brief Reads every entitlement.
  ///
  /// Blocks for every page in turn; each request is bounded by the HTTP
  /// provider's timeouts and retries and abandoned when the stop token fires.
  /// Logs the counts and the platform and package kinds read.
  ///
  /// @param pages Told each page read; the total is not known up front.
  /// @return The records; the read's failure otherwise.
  [[nodiscard]] auto all(PageCount pages = {}) -> Expected<Listed, Unanswered>;
  /// @brief Reads the concepts of the account's streamed PS4 copies (package
  /// type PS4GS).
  ///
  /// Blocks for every page in turn.
  ///
  /// @param pages Told each page read and the page total.
  /// @return The concepts learned, by entitlement and product; the read's
  ///     failure otherwise.
  [[nodiscard]] auto concepts_of_streamed_ps4_copies(PageCount pages = {})
      -> Expected<std::vector<Library::LearnedConcept>, Unanswered>;
  /// @brief Re-reads the first of several entitlements the account holds.
  ///
  /// Asks about each distinct id in turn, at most lookup_limit of them, and
  /// stops at the first held one or the first failure.
  ///
  /// @param ids The entitlement ids, in the order to ask.
  /// @return The first held record; std::nullopt when none asked is held;
  ///     Unanswered::Reason::malformed when a held record is missing or does
  ///     not parse; otherwise the first failure.
  [[nodiscard]] auto one_of(std::span<const OwnershipId> ids)
      -> Expected<std::optional<OwnedRecord>, Unanswered>;

  /// @brief Whether a streamed PS4 copy came without a concept, so the concepts
  /// should be read separately.
  ///
  /// @param records The records.
  /// @return True when any PS4GS record has no concept.
  static auto lacks_a_concept(std::span<const OwnedRecord> records) noexcept
      -> bool;
  /// @brief A log summary of the records' platforms and package types.
  ///
  /// @param records The records.
  /// @return "platform package xN" entries, comma-separated and sorted.
  static auto kinds_of(std::span<const OwnedRecord> records) -> std::string;

 private:
  /// @brief A tolerant text field.
  using Text = json::ps::Text;
  /// @brief A field that is read when it has the expected shape and is empty
  /// otherwise.
  ///
  /// @tparam Value The expected shape.
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;

  /// @brief An entitlement's gameMeta block. Plain data.
  struct GameMeta {
    /// @brief The game's name.
    Text name;
    /// @brief The package type, such as PS4GD or PS4GS.
    Text package_type;
    /// @brief The SKU type.
    Text sku_type;
    /// @brief The icon URL.
    Text icon_url;
    /// @brief The image URL.
    Text image_url;
    /// @brief The concept id, as a string or a number.
    std::optional<glz::raw_json> concept_id;
    /// @brief The images.
    ImageList images;

    /// @brief How the block is read: snake_case keys with a few camelCase ones.
    struct glaze {
      /// @brief The type described.
      using T = GameMeta;
      /// @brief The keys read.
      static constexpr auto value = glz::object(
          "name", &T::name, "package_type", &T::package_type, "sku_type",
          &T::sku_type, "icon_url", &T::icon_url, "imageUrl", &T::image_url,
          "conceptId", &T::concept_id, "images", &T::images);
    };
  };
  /// @brief One entitlement attribute. Plain data.
  struct Attribute {
    /// @brief The platform the attribute names, such as ps4.
    Text platform_id;
  };
  /// @brief An entitlement's skuMeta block. Plain data.
  struct SkuMeta {
    /// @brief The playability date, as sent.
    std::optional<glz::raw_json> playability_date;
  };
  /// @brief One commerce entitlement as PS sends it. Plain data.
  struct Granted {
    /// @brief The entitlement id.
    Text id;
    /// @brief The product it was bought under.
    Text product_id;
    /// @brief The record's own name, used without gameMeta.
    Text name;
    /// @brief The gameMeta block; absent for non-games.
    Tolerant<GameMeta> game_meta;
    /// @brief The skuMeta block.
    Tolerant<SkuMeta> sku_meta;
    /// @brief Whether the entitlement is active.
    Tolerant<bool> active_flag;
    /// @brief The numeric feature type.
    Tolerant<json::ps::Number> feature_type;
    /// @brief The SKU type.
    Text sku_type;
    /// @brief The attributes, naming platforms.
    StoreList<Attribute> entitlement_attributes;
    /// @brief The active date, as sent.
    std::optional<glz::raw_json> active_date;
    /// @brief The preorder flag.
    Tolerant<bool> preorder_flag;
    /// @brief The record's images.
    ImageList images;
    /// @brief The record's image URL.
    Text image_url;
    /// @brief The SKU id.
    Text sku_id;
    /// @brief The camelCase conceptId.
    std::optional<glz::raw_json> concept_id;
    /// @brief The snake_case concept_id, read when conceptId is absent.
    std::optional<glz::raw_json> snake_concept_id;

    /// @brief How the record is read: snake_case keys with a few camelCase
    /// ones.
    struct glaze {
      /// @brief The type described.
      using T = Granted;
      /// @brief The keys read.
      static constexpr auto value = glz::object(
          "id", &T::id, "product_id", &T::product_id, "name", &T::name,
          "game_meta", &T::game_meta, "sku_meta", &T::sku_meta, "active_flag",
          &T::active_flag, "feature_type", &T::feature_type, "sku_type",
          &T::sku_type, "entitlement_attributes", &T::entitlement_attributes,
          "active_date", &T::active_date, "preorder_flag", &T::preorder_flag,
          "images", &T::images, "imageUrl", &T::image_url, "sku_id", &T::sku_id,
          "conceptId", &T::concept_id, "concept_id", &T::snake_concept_id);
    };
  };

  /// @brief Reads one entitlement.
  ///
  /// The name, package and SKU type prefer gameMeta; the cover prefers the
  /// gameMeta icon, then gameMeta images, then the record's images; the concept
  /// prefers conceptId, then concept_id, then gameMeta's.
  ///
  /// @param entitlement The entitlement as sent.
  /// @return The record, or std::nullopt when it is not an object or does not
  ///     parse.
  static auto record_of(const glz::raw_json& entitlement)
      -> std::optional<OwnedRecord>;
  /// @brief The first platform the attributes name.
  ///
  /// @param attributes The attributes.
  /// @return "ps5", "ps4" or "ps3", lower-cased; empty when none names one.
  static auto platform_of(const StoreList<Attribute>& attributes)
      -> std::string;

  /// @brief The user; not owned.
  users::PsUser& user_;
  /// @brief Sends requests; not owned.
  net::http::Client& http_;
  /// @brief Stamps reads.
  WallStamp stamp_;
  /// @brief The largest reply accepted, in bytes.
  std::size_t maximum_response_bytes_;
};

}  // namespace libchiaking::catalog::ps
