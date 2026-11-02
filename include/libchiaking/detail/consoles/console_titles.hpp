/// @file
/// @brief Declares InstalledTitle and the outcomes of listing and starting
/// titles on a console through PSN.
#pragma once

#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/users/ps_user.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::consoles {

class Console;

/// @brief A game installed on a console, as PSN describes it for an account.
///
/// Values come from Console::titles and KnownConsoles::recently_played and are
/// passed back to Console::start. A title is immutable once created.
class InstalledTitle {
 public:
  /// @brief Longest title id or start id accepted, in bytes.
  static constexpr std::size_t id_limit = 64;
  /// @brief Longest concept id accepted, in digits.
  static constexpr std::size_t concept_limit = 24;

  /// @brief When a title was last used, and whose record that time comes
  /// from.
  struct LastUse {
    /// @brief Which record a last-use time was taken from.
    enum class Known : std::uint8_t {
      /// @brief The console's own record of when the title was last
      /// opened on it.
      on_this_console,
      /// @brief The account's record of when the title was last played,
      /// on any console. Used when the console has no record of its own.
      on_the_account,
    };

    /// @brief The moment of last use.
    std::chrono::system_clock::time_point at{};
    /// @brief Which record `at` comes from.
    Known known{Known::on_this_console};

    /// @brief Compares both fields.
    friend auto operator==(const LastUse&, const LastUse&) noexcept
        -> bool = default;
  };

  /// @brief The plain facts an InstalledTitle is created from.
  struct Facts {
    /// @brief PSN's identifier for the title.
    std::string title_id;
    /// @brief The identifier sent to PSN to start the title: the title id
    /// without a trailing "_00".
    std::string start_id;
    /// @brief The title's display name.
    std::string name;
    /// @brief Address of the title's cover image.
    std::string cover_url;
    /// @brief Whether the title is a PS4 or a PS5 game. A PS5 can hold
    /// both.
    Generation platform{Generation::ps5};
    /// @brief When the title was last used, when PSN states it.
    std::optional<LastUse> last_used{};
    /// @brief Whether the title is among the account's most recently
    /// played.
    bool recently_played{};
    /// @brief PSN's concept id for the title, as decimal digits. Empty when
    /// PSN did not state a usable one.
    std::string concept_id{};

    /// @brief Compares every field.
    friend auto operator==(const Facts&, const Facts&) noexcept
        -> bool = default;
  };

  /// @brief Validates the facts and builds a title from them.
  ///
  /// @param facts The facts to adopt.
  /// @return The title, or nothing when the title id or start id is empty,
  ///     longer than id_limit or holds a control character, when the name or
  ///     cover address is blank, or when the concept id is longer than
  ///     concept_limit or holds anything but digits.
  [[nodiscard]] static auto create(Facts facts)
      -> std::optional<InstalledTitle>;

  /// @brief PSN's identifier for the title.
  ///
  /// @return The title id, never empty.
  auto title_id() const noexcept -> const std::string& {
    return facts_.title_id;
  }
  /// @brief The identifier Console::start sends to PSN to start the title.
  ///
  /// @return The start id, never empty.
  auto start_id() const noexcept -> const std::string& {
    return facts_.start_id;
  }
  /// @brief The title's display name.
  ///
  /// @return The name, never blank.
  auto name() const noexcept -> const std::string& { return facts_.name; }
  /// @brief Address of the title's cover image.
  ///
  /// @return The cover address, never blank.
  auto cover_url() const noexcept -> const std::string& {
    return facts_.cover_url;
  }
  /// @brief Whether the title is a PS4 or a PS5 game.
  ///
  /// @return Generation::ps4 or Generation::ps5.
  auto platform() const noexcept -> Generation { return facts_.platform; }
  /// @brief When the title was last used and whose record says so, if PSN
  /// stated either.
  ///
  /// @return The use, or nothing when PSN stated none.
  auto last_used() const noexcept -> const std::optional<LastUse>& {
    return facts_.last_used;
  }
  /// @brief PSN's concept id for the title as decimal digits, or empty.
  ///
  /// @return Decimal digits, or an empty string when PSN gave none.
  auto concept_id() const noexcept -> const std::string& {
    return facts_.concept_id;
  }
  /// @brief Whether the title is among the account's most recently played.
  ///
  /// @return True when the title is among the most recently played, false
  /// otherwise.
  auto recently_played() const noexcept -> bool {
    return facts_.recently_played;
  }

  /// @brief Compares every fact.
  friend auto operator==(const InstalledTitle&, const InstalledTitle&) noexcept
      -> bool = default;

 private:
  explicit InstalledTitle(Facts facts) noexcept;

  static auto valid_id(std::string_view id) noexcept -> bool;
  static auto valid_concept(std::string_view digits) noexcept -> bool;

  Facts facts_;
};

/// @brief The recently played titles installed on one console.
///
/// @see KnownConsoles::recently_played
struct RecentlyPlayed {
  /// @brief The known console the titles are installed on.
  std::shared_ptr<Console> console;
  /// @brief That console's recently played titles, most recent first.
  std::vector<InstalledTitle> titles;
};

/// @brief How far a request to start a title has got, as PSN reports it.
enum class StartState : std::uint8_t {
  /// @brief PSN accepted the command and the console has not acted on it yet.
  pending,
  /// @brief PSN reports the command as being carried out.
  starting,
  /// @brief PSN reports the title as started.
  completed,
  /// @brief PSN reports that the console could not start the title.
  failed,
};

/// @brief Receives the states a title start passes through.
///
/// @see Console::start
class StartWatcher {
 public:
  /// @brief Allows deletion through the interface.
  virtual ~StartWatcher() = default;
  /// @brief Called on the thread that called Console::start each time the
  /// state changes.
  ///
  /// @param state The state just reached. StartState::pending is reported
  ///     first.
  virtual void on_state(StartState state) = 0;
};

/// @brief Why a console's installed titles could not be listed.
struct TitlesUnavailable {
  /// @brief The kinds of failure.
  enum class Reason : std::uint8_t {
    /// @brief The console is a PS4; PSN lists installed titles for PS5
    /// only.
    ps5_only,
    /// @brief The user holds no pairing with the console.
    not_paired,
    /// @brief None of the consoles PSN lists for the account could be
    /// matched to this one.
    not_on_account,
    /// @brief PSN did not answer; see TitlesUnavailable::unanswered.
    unanswered,
    /// @brief The console has been forgotten.
    forgotten,
    /// @brief No more specific reason. This is the default value.
    not_available,
  };

  /// @brief What went wrong.
  Reason reason{Reason::not_available};
  /// @brief PSN's failure, set when the reason is Reason::unanswered.
  std::optional<users::Unanswered> unanswered{};
};

/// @brief Why a title was not started on a console.
struct StartFailure {
  /// @brief The kinds of failure.
  enum class Reason : std::uint8_t {
    /// @brief The console is a PS4; titles can be started through PSN on
    /// PS5 only.
    ps5_only,
    /// @brief The user holds no pairing with the console.
    not_paired,
    /// @brief The console could not be matched to one PSN lists for the
    /// account.
    not_on_account,
    /// @brief PSN rejected the request with a client error other than an
    /// authorisation refusal.
    refused,
    /// @brief Neither a completed nor a failed state arrived before the
    /// deadline.
    timed_out,
    /// @brief The stop token was triggered.
    cancelled,
    /// @brief PSN did not answer; see StartFailure::unanswered.
    unanswered,
    /// @brief The console has been forgotten.
    forgotten,
    /// @brief No more specific reason. This is the default value.
    not_available,
  };

  /// @brief What went wrong.
  Reason reason{Reason::not_available};
  /// @brief The last state seen before the failure.
  StartState reached{StartState::pending};
  /// @brief PSN's failure, when one was involved. Set for Reason::unanswered,
  /// Reason::refused and a cancellation noticed by a PSN request.
  std::optional<users::Unanswered> unanswered{};
};

}  // namespace libchiaking::consoles
