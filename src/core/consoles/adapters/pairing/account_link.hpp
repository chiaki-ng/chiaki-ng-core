/// @file
/// @brief Declares AccountLink, which works out which of the user's paired
/// PS5 consoles is which console on the user's PSN account, and records the
/// link by PSN's device uid.
///
/// A PS5 is reached over the internet by the device uid PSN lists it under,
/// but a console paired at home is known only by its MAC address. When the
/// account's consoles are listed, each paired console is matched to a listed
/// one by conservative rules that never guess between two candidates, so a
/// later internet start goes to the right console.
#pragma once

#include "libchiaking/detail/consoles/account_console.hpp"
#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/users/ps_user.hpp"
#include "src/core/consoles/adapters/roster/console_facts.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace libchiaking::platform {
class DiagnosticsProvider;
}

namespace libchiaking::consoles {

class KnownConsoles;

/// @brief Matches the user's paired PS5 consoles to the consoles PSN lists
/// for the account, and saves each new match as an account link.
///
/// Holds only a ConsoleFacts and a log; it keeps no state between calls and
/// may be used from several threads at once. The roster it records in must
/// outlive it.
class AccountLink {
 public:
  /// @brief Why a paired console was taken to be a listed one.
  enum class Rule : std::uint8_t {
    /// @brief The console's pairing already holds a device uid that no
    /// other paired console holds, and exactly one listed console carries
    /// it. Nothing new is saved.
    stored_device_uid,
    /// @brief The console has no device uid yet, no other unlinked paired
    /// console has the same name, and exactly one listed console has that
    /// name, compared after trimming and folding ASCII case.
    unique_name,
    /// @brief Nothing else matched, the user has exactly one paired console
    /// and PSN lists exactly one, and the paired console has no device uid
    /// yet.
    sole_console,
  };

  /// @brief A paired console as the matching sees it.
  struct Saved {
    /// @brief The console's display name.
    std::string name;
    /// @brief The device uid already linked to the user's pairing, if any.
    std::optional<DeviceUid> device_uid{};
  };

  /// @brief A console PSN lists for the account, as the matching sees it.
  struct Listed {
    /// @brief The name PSN holds for the console.
    std::string name;
    /// @brief The device uid PSN lists the console under; empty when it
    /// gave none usable.
    std::optional<DeviceUid> device_uid{};
  };

  /// @brief One paired console matched to one listed console.
  struct Match {
    /// @brief Index of the paired console in the `saved` span given to
    /// matched().
    std::size_t saved{};
    /// @brief Index of the listed console in the `listed` span given to
    /// matched().
    std::size_t listed{};
    /// @brief The rule that matched them.
    Rule rule{Rule::stored_device_uid};

    /// @brief Compares the indices and the rule.
    friend auto operator==(const Match&, const Match&) noexcept
        -> bool = default;
  };

  /// @brief A paired console linked to a listed console.
  struct Link {
    /// @brief The paired console; never null.
    std::shared_ptr<Console> console;
    /// @brief Index of the listed console in the `listed` span given to
    /// linked().
    std::size_t listed{};
    /// @brief The rule that matched them.
    Rule rule{Rule::stored_device_uid};
  };

  /// @brief Links consoles in a registry's roster.
  ///
  /// @param known The registry; its roster must outlive this object.
  /// @param log Where a link that could not be saved is logged; null logs
  ///     nothing. Must outlive this object.
  explicit AccountLink(KnownConsoles& known,
                       platform::DiagnosticsProvider* log = nullptr) noexcept;
  /// @brief Links consoles in a roster.
  ///
  /// @param roster The roster; must outlive this object.
  /// @param log Where a link that could not be saved is logged; null logs
  ///     nothing. Must outlive this object.
  explicit AccountLink(Roster& roster,
                       platform::DiagnosticsProvider* log = nullptr) noexcept;

  /// @brief Matches the user's paired PS5 consoles to the listed ones and
  /// records what each match teaches.
  ///
  /// Only PS5 consoles the user is paired with and that are not forgotten
  /// take part. For a match made by name or as the sole console, the listed
  /// device uid is saved on the user's pairing; a refused save is logged as a
  /// warning and the match is still returned. Every matched console also
  /// learns the listed name with ConsoleName::Source::account, which replaces
  /// only a placeholder or an earlier account name. Blocks while the store is
  /// written.
  ///
  /// @param user The user whose pairings are matched.
  /// @param listed The consoles PSN lists for the user's account.
  /// @return The matches, by rule in the order matched() finds them; empty
  ///     when nothing matched.
  auto linked(const users::PsUser& user, std::span<const Listed> listed)
      -> std::vector<Link>;

  /// @brief Pairs paired consoles with listed consoles by the rules, without
  /// touching anything.
  ///
  /// The rules are tried in order: stored_device_uid for every paired
  /// console, then unique_name for those left, then sole_console. Each paired
  /// and each listed console is used at most once, and a candidate that is
  /// not the only one is never taken.
  ///
  /// @param saved The user's paired consoles.
  /// @param listed The consoles PSN lists.
  /// @return The matches; empty when nothing matched.
  static auto matched(std::span<const Saved> saved,
                      std::span<const Listed> listed) -> std::vector<Match>;
  /// @brief Turns the account's consoles into what the matching compares.
  ///
  /// @param consoles The consoles AccountConsoles listed.
  /// @return Their names and device uids, in the same order.
  static auto listed(std::span<const AccountConsole> consoles)
      -> std::vector<Listed>;

 private:
  /// @brief Puts a name in the form names are compared in: surrounding
  /// whitespace trimmed, ASCII letters lowercased.
  ///
  /// @param name The name.
  /// @return The folded name; empty for a blank name, which never matches.
  static auto folded(std::string_view name) -> std::string;

  /// @brief Where links and names are recorded.
  ConsoleFacts facts_;
  /// @brief Where a link that could not be saved is logged; null when nothing
  /// is logged.
  platform::DiagnosticsProvider* log_;
};

}  // namespace libchiaking::consoles
