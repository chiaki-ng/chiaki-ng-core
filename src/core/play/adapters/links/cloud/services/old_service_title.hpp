/// @file
/// @brief Declares OldServiceTitle, a LegacyCloudStream store title resolved to
/// the entitlement, SKU and platform a launch uses, and the store reply it is
/// read from.
#pragma once

#include "src/core/psn/gk/datacenter_ping.hpp"
#include "src/core/utils/json/ps_reply.hpp"

#include <glaze/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::play {

/// @brief A LegacyCloudStream title as a launch uses it: the entitlement, the
/// SKU to take the licence with, the platform it streams as, and whether a
/// PlayStation Plus plan covers it.
///
/// A value; the entitlement and SKU are cut to their wire limits.
class OldServiceTitle {
  /// @brief A store field that may be missing or of the wrong type.
  ///
  /// @tparam Value The field's type.
  template <class Value>
  using Tolerant = json::ps::Tolerant<Value>;
  /// @brief A store list whose entries may each be unusable.
  ///
  /// @tparam Value The entries' type.
  template <class Value>
  using Listed = Tolerant<std::vector<Tolerant<Value>>>;
  /// @brief A store text field.
  using Text = Tolerant<std::string>;

 public:
  /// @brief One entitlement of a SKU.
  struct Entry {
    /// @brief The entitlement id.
    Text id;
    /// @brief The licence type; 4 marks the streaming licence.
    Tolerant<json::ps::Number> license_type;
    /// @brief The package type; one ending "GS" or "GD" is a game.
    Text package_type;

    /// @brief The JSON field names.
    struct glaze {
      /// @brief The type described.
      using T = Entry;
      /// @brief The field mapping.
      static constexpr auto value =
          glz::object("id", &T::id, "license_type", &T::license_type,
                      "packageType", &T::package_type);
    };
  };
  /// @brief One SKU of the title.
  struct Sku {
    /// @brief The SKU id.
    Text id;
    /// @brief Its entitlements.
    Listed<Entry> entitlements;
  };
  /// @brief A list of text values.
  struct Values {
    /// @brief The values.
    Listed<std::string> values;
  };
  /// @brief The title's metadata.
  struct Metadata {
    /// @brief The platforms it plays on.
    Tolerant<Values> playable_platform;
  };
  /// @brief What the store says of one SKU's offers.
  struct Relevancy {
    /// @brief The JSON names are camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief The SKU id.
    Text sku_id;
    /// @brief The plans that offer it; empty when none.
    Tolerant<std::vector<glz::raw_json>> offer_availability;
  };
  /// @brief The title's offers.
  struct Offers {
    /// @brief The JSON names are camel case.
    using keys = json::ps::CamelCaseKeys;
    /// @brief One entry per SKU.
    Listed<Relevancy> sku_relevancy;
  };
  /// @brief The store's title container.
  struct Container {
    /// @brief The default SKU.
    Tolerant<Sku> default_sku;
    /// @brief The other SKUs.
    Listed<Sku> skus;
    /// @brief The platforms it plays on.
    Listed<std::string> playable_platform;
    /// @brief The title's metadata.
    Tolerant<Metadata> metadata;
    /// @brief The title's offers.
    Tolerant<Offers> offers;
  };
  /// @brief The store reply.
  using Reply = Tolerant<Container>;

  /// @enum PlanCover
  /// @brief Whether a PlayStation Plus plan offers the SKU.
  ///
  /// @var PlanCover::unstated
  /// @brief The store does not say.
  ///
  /// @var PlanCover::covered
  /// @brief At least one plan offers it.
  ///
  /// @var PlanCover::absent
  /// @brief The store lists no plan for it.
  enum class PlanCover : std::uint8_t { unstated, covered, absent };

  /// @brief The longest SKU kept, in bytes.
  static constexpr std::size_t sku_limit = 159;

  /// @brief Builds a title with a known entitlement and no SKU.
  ///
  /// @param entitlement The entitlement, cut to the GK id limit.
  /// @param platform The platform it streams as.
  OldServiceTitle(std::string_view entitlement,
                  psn::StreamingPlatform platform);

  /// @brief Reads a title from the store's reply.
  ///
  /// The entitlement is the first with the streaming licence type, else the
  /// first game package whose id names the title, else any game package
  /// (GS before GD).
  ///
  /// @param container The store reply.
  /// @param identifier The store product id.
  /// @return The title; empty when the reply is unusable or names no
  ///     entitlement.
  static auto decode(const Reply& container, std::string_view identifier)
      -> std::optional<OldServiceTitle>;
  /// @brief Reports whether a product is a PS3 or older classic.
  ///
  /// @param identifier The store product id.
  /// @return `true` unless its title id starts with CUSA or PPSA.
  static auto is_legacy(std::string_view identifier) -> bool;

  /// @brief Reads the entitlement.
  ///
  /// @return The entitlement id.
  auto entitlement() const noexcept -> const std::string& {
    return entitlement_;
  }
  /// @brief Reads the SKU.
  ///
  /// @return The SKU; empty when unknown.
  auto sku() const noexcept -> const std::string& { return sku_; }
  /// @brief Reads the platform.
  ///
  /// @return The platform it streams as.
  auto platform() const noexcept -> psn::StreamingPlatform { return platform_; }
  /// @brief Reports whether no plan offers the SKU.
  ///
  /// @return `true` when the store lists no plan.
  auto is_outside_every_plan() const noexcept -> bool {
    return plan_cover_ == PlanCover::absent;
  }
  /// @brief Gives the id to take the licence with.
  ///
  /// @return The SKU, or the entitlement when there is none.
  auto checkout_sku() const noexcept -> std::string_view;
  /// @brief Replaces the SKU, cutting it to OldServiceTitle::sku_limit.
  ///
  /// @param sku The SKU.
  void replace_sku(std::string_view sku);

 private:
  /// @brief An entitlement and the SKU it belongs to.
  struct Candidate {
    /// @brief The entitlement id.
    std::string_view entitlement;
    /// @brief The SKU id.
    std::string_view sku;
  };

  /// @brief Reads a text field.
  ///
  /// @param value The field.
  /// @return Its text; empty when missing.
  static auto text_of(const Text& value) -> std::string_view;
  /// @brief Upper-cases ASCII.
  ///
  /// @param value The text.
  /// @return The upper-cased copy.
  static auto upper(std::string_view value) -> std::string;
  /// @brief Picks the entitlement to launch.
  ///
  /// @param container The store container.
  /// @param identifier The store product id.
  /// @return The entitlement and SKU, pointing into @p container; empty when
  ///     none qualifies.
  static auto select(const Container& container, std::string_view identifier)
      -> std::optional<Candidate>;
  /// @brief Picks the platform the title streams as.
  ///
  /// @param container The store container.
  /// @return PS5, PS4 or PS3 by the playable platforms, PS4 when unstated.
  static auto playable_platform(const Container& container)
      -> psn::StreamingPlatform;
  /// @brief Reads whether a plan offers a SKU.
  ///
  /// @param container The store container.
  /// @param sku The SKU.
  /// @return Whether it is covered, absent or unstated.
  static auto plan_cover(const Container& container, std::string_view sku)
      -> PlanCover;

  /// @brief The entitlement id.
  std::string entitlement_;
  /// @brief The SKU; empty when unknown.
  std::string sku_;
  /// @brief The platform it streams as.
  psn::StreamingPlatform platform_;
  /// @brief Whether a plan offers the SKU.
  PlanCover plan_cover_{PlanCover::unstated};
};

}  // namespace libchiaking::play
