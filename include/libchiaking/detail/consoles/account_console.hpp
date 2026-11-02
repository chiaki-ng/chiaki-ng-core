/// @file
/// @brief Declares AccountConsole, a console as PSN lists it on an account.
#pragma once

#include "libchiaking/detail/consoles/device_uid.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace libchiaking::consoles {

/// @brief A console registered to a PSN account, as PSN describes it.
///
/// This is what KnownConsoles::consoles_on_account returns and what
/// KnownConsoles::pair_over_the_internet takes. It carries PSN's own identifier
/// for the console, not a HardwareId, so it says nothing about whether the
/// console is known or paired here.
///
/// PSN lists PS5 consoles individually. It does not list PS4 consoles, so a PS4
/// account is represented by the single placeholder main_ps4().
///
/// @see KnownConsoles::consoles_on_account
/// @see DeviceUid
class AccountConsole {
 public:
  /// @brief Longest name accepted, in bytes.
  static constexpr std::size_t name_limit = 512;
  /// @brief Name given to the placeholder returned by main_ps4().
  static constexpr std::string_view main_ps4_name = "Main PS4 Console";

  /// @brief Builds an account console from the facts PSN states.
  ///
  /// @param device_uid PSN's identifier for the console.
  /// @param name Name PSN gives the console. May be empty.
  /// @param generation Whether PSN lists it as a PS4 or a PS5.
  /// @param remote_play_enabled Whether PSN reports Remote Play as switched
  ///     on for the console.
  /// @return The console, or nothing when the name is longer than name_limit,
  ///     the name holds a control character, or the main PS4 identifier is
  ///     combined with Generation::ps5.
  [[nodiscard]] static auto create(DeviceUid device_uid, std::string name,
                                   Generation generation,
                                   bool remote_play_enabled)
      -> std::optional<AccountConsole>;
  /// @brief Returns the placeholder that stands for the account's main PS4.
  ///
  /// It carries DeviceUid::main_ps4(), the name main_ps4_name,
  /// Generation::ps4 and Remote Play reported as enabled.
  ///
  /// @return The placeholder console; never null or empty.
  static auto main_ps4() -> AccountConsole;

  /// @brief PSN's identifier for the console, used to address it over the
  /// internet.
  ///
  /// @return The identifier, which lives as long as this console record.
  auto device_uid() const noexcept -> const DeviceUid& { return device_uid_; }
  /// @brief The name PSN holds for the console. May be empty.
  ///
  /// @return The name, which lives as long as this console record; empty when
  /// PSN holds none.
  auto name() const noexcept -> const std::string& { return name_; }
  /// @brief Whether PSN lists the console as a PS4 or a PS5.
  ///
  /// @return Generation::ps4 or Generation::ps5.
  auto generation() const noexcept -> Generation { return generation_; }
  /// @brief Whether PSN reports Remote Play as switched on for the console.
  ///
  /// KnownConsoles::pair_over_the_internet refuses a console for which this
  /// is false with PairingFailure::Reason::remote_play_disabled.
  ///
  /// @return True when PSN reports Remote Play as enabled, false when it
  /// reports it disabled.
  auto remote_play_enabled() const noexcept -> bool {
    return remote_play_enabled_;
  }
  /// @brief Whether this is the main PS4 placeholder rather than a listed
  /// console.
  ///
  /// @return True for the placeholder made by main_ps4(), false for a console
  /// PSN listed.
  auto is_main_ps4() const noexcept -> bool {
    return device_uid_.is_main_ps4();
  }

  /// @brief Compares every field.
  friend auto operator==(const AccountConsole&, const AccountConsole&) noexcept
      -> bool = default;

 private:
  AccountConsole(DeviceUid device_uid, std::string name, Generation generation,
                 bool remote_play_enabled) noexcept
      : device_uid_(device_uid),
        name_(std::move(name)),
        generation_(generation),
        remote_play_enabled_(remote_play_enabled) {}

  DeviceUid device_uid_;
  std::string name_;
  Generation generation_;
  bool remote_play_enabled_;
};

}  // namespace libchiaking::consoles
