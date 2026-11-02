/// @file
/// @brief Declares StreamRefusal, a remembered refusal by PS's streaming
/// service to start a title.
#pragma once

#include "libchiaking/detail/catalog/owned_title.hpp"
#include "libchiaking/detail/catalog/product_id.hpp"
#include "libchiaking/detail/catalog/wall_time.hpp"
#include "libchiaking/detail/users/account_id.hpp"
#include "libchiaking/detail/users/membership.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <variant>

namespace libchiaking::catalog {

/// @brief A refusal by PS's streaming service to start a title, remembered so
/// the same launch is not offered again at once.
///
/// A refusal stands for StreamRefusal::lifetime, and only while the
/// circumstances it was made under still hold: the same tier, and no known
/// change in whether the user holds the title.
///
/// @see Catalog::remember_refusal
/// @see LaunchRefusal::Kind::refused_by_service
class StreamRefusal {
 public:
  /// @brief Wall-clock time in milliseconds.
  using Time = catalog::Time;

  /// @brief What the streaming service said.
  enum class Kind : std::uint8_t {
    /// @brief The service found no game for the user's entitlement, which
    /// puts the held library in doubt.
    no_game_for_entitlement,
    /// @brief The streaming server refused for any other reason.
    refused_by_stream_server,
    /// @brief PS refused a launch of a trial in a way that means the
    /// trial cannot be played any more: the launch-time re-read of the
    /// trial's entitlement found it inactive or its play time used up.
    /// While it stands the trial is listed as ended.
    trial_ended,
  };

  /// @brief Whether the user held the refused title, as far as was known.
  class Holding {
   public:
    /// @brief What is known.
    enum class State : std::uint8_t {
      /// @brief The library could not be read.
      unknown,
      /// @brief The user does not hold the title.
      not_held,
      /// @brief The user holds the title.
      held,
    };

    /// @brief Builds an unknown holding.
    Holding() = default;

    /// @brief Builds the holding for a library that could not be read.
    ///
    /// @return A holding whose state is unknown.
    static auto unknown() noexcept -> Holding { return Holding{}; }
    /// @brief Builds the holding for a title the user is known not to hold.
    ///
    /// @return A holding whose state is not held.
    static auto not_held() noexcept -> Holding { return Holding{NotHeld{}}; }
    /// @brief Builds the holding for a title the user holds.
    ///
    /// @param id The entitlement it is held as.
    /// @return The holding.
    static auto held_as(OwnershipId id) -> Holding {
      return Holding{std::move(id)};
    }
    /// @brief Builds a holding from what a game knows about its ownership.
    ///
    /// @param ownership The user's copy, when there is one.
    /// @param ownership_checked Whether the library was read.
    /// @return Held when a copy is given; otherwise not held when the
    ///     library was read, and unknown when it was not.
    static auto of(const std::optional<OwnedTitle>& ownership,
                   bool ownership_checked) -> Holding {
      if (ownership) return held_as(ownership->id());
      return ownership_checked ? not_held() : unknown();
    }

    /// @brief What is known about the holding.
    ///
    /// @return Whether the holding is unknown, not held or held.
    auto state() const noexcept -> State {
      return static_cast<State>(known_.index());
    }
    /// @brief Whether the library was read, so the holding is certain.
    ///
    /// @return True unless the state is unknown.
    auto is_known() const noexcept -> bool { return state() != State::unknown; }
    /// @brief Whether the user holds the title.
    ///
    /// @return True when the state is held; false otherwise.
    auto is_held() const noexcept -> bool { return state() == State::held; }
    /// @brief The entitlement the title is held as.
    ///
    /// @return The entitlement, or null unless the title is held. The
    ///     pointer lives as long as this holding.
    auto id() const noexcept -> const OwnershipId* {
      return std::get_if<OwnershipId>(&known_);
    }
    /// @brief Whether the holding is known to differ from a later one.
    ///
    /// @param now The holding at present.
    /// @return True only when both holdings are known and differ; an
    ///     unknown holding on either side never counts as a change.
    auto has_changed_to(const Holding& now) const noexcept -> bool {
      return is_known() && now.is_known() && *this != now;
    }

    /// @brief Compares state and entitlement.
    friend auto operator==(const Holding&, const Holding&) -> bool = default;

   private:
    struct Unknown {
      friend auto operator==(Unknown, Unknown) -> bool = default;
    };
    struct NotHeld {
      friend auto operator==(NotHeld, NotHeld) -> bool = default;
    };
    using Known = std::variant<Unknown, NotHeld, OwnershipId>;
    static_assert(std::variant_size_v<Known> ==
                  static_cast<std::size_t>(State::held) + 1);

    explicit Holding(Known known) noexcept : known_(std::move(known)) {}

    Known known_{};
  };

  /// @brief The conditions a refusal was made under.
  struct Circumstances {
    /// @brief The user's tier; empty when it was not known.
    std::optional<users::Tier> tier{};
    /// @brief Whether the user held the title.
    Holding holding{};

    /// @brief Compares tier and holding.
    friend auto operator==(const Circumstances&, const Circumstances&)
        -> bool = default;
  };

  /// @brief How long a refusal stands.
  static constexpr std::chrono::hours lifetime{24};

  /// @brief Records a refusal.
  ///
  /// @param account The account that was refused.
  /// @param product The title that was refused.
  /// @param kind What the streaming service said.
  /// @param refused_at When it refused.
  /// @param circumstances The tier and holding at that moment.
  StreamRefusal(AccountId account, ProductId product, Kind kind,
                Time refused_at, Circumstances circumstances) noexcept
      : account_(account),
        product_(std::move(product)),
        kind_(kind),
        refused_at_(refused_at),
        circumstances_(std::move(circumstances)) {}

  /// @brief The account that was refused.
  ///
  /// @return The account that was refused.
  auto account() const noexcept -> AccountId { return account_; }
  /// @brief The title that was refused.
  ///
  /// @return The refused product; valid as long as this object.
  auto product_id() const noexcept -> const ProductId& { return product_; }
  /// @brief What the streaming service said.
  ///
  /// @return The kind of refusal.
  auto kind() const noexcept -> Kind { return kind_; }
  /// @brief When the service refused.
  ///
  /// @return The time of the refusal.
  auto refused_at() const noexcept -> Time { return refused_at_; }
  /// @brief The tier and holding at the moment of refusal.
  ///
  /// @return The recorded circumstances; valid as long as this object.
  auto circumstances() const noexcept -> const Circumstances& {
    return circumstances_;
  }

  /// @brief When the refusal stops standing, StreamRefusal::lifetime after it
  /// was made.
  ///
  /// @return The refusal time plus the refusal lifetime.
  auto expires_at() const noexcept -> Time { return refused_at_ + lifetime; }
  /// @brief Whether the refusal suggests the held library is wrong, which is
  /// the case for StreamRefusal::Kind::no_game_for_entitlement.
  ///
  /// @return True when the kind is Kind::no_game_for_entitlement; false
  /// otherwise.
  auto questions_ownership() const noexcept -> bool {
    return kind_ == Kind::no_game_for_entitlement;
  }
  /// @brief Whether the refusal says a trial has ended, which is the case for
  /// StreamRefusal::Kind::trial_ended.
  ///
  /// @return True when the kind is Kind::trial_ended; false otherwise.
  auto ends_the_trial() const noexcept -> bool {
    return kind_ == Kind::trial_ended;
  }
  /// @brief Whether the refusal should still keep the title from launching.
  ///
  /// @param now The current wall-clock time.
  /// @param current The user's tier and holding at present.
  /// @return True when now lies between StreamRefusal::refused_at and
  ///     StreamRefusal::expires_at, the tier is the same, and the holding has
  ///     not changed in the sense of Holding::has_changed_to.
  auto still_stands(Time now, const Circumstances& current) const noexcept
      -> bool {
    return now >= refused_at_ && now < expires_at() &&
           current.tier == circumstances_.tier &&
           !circumstances_.holding.has_changed_to(current.holding);
  }

  /// @brief Compares every field.
  friend auto operator==(const StreamRefusal&, const StreamRefusal&)
      -> bool = default;

 private:
  AccountId account_;
  ProductId product_;
  Kind kind_;
  Time refused_at_;
  Circumstances circumstances_;
};

}  // namespace libchiaking::catalog
