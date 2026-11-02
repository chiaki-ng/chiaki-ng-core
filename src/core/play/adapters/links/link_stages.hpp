/// @file
/// @brief Declares LinkStages, the table that names each stage a link goes
/// through and maps it to diagnostics, end reasons, connect and pairing
/// steps.
#pragma once

#include "libchiaking/detail/consoles/pairing.hpp"
#include "libchiaking/detail/play/outcome.hpp"
#include "libchiaking/detail/play/session_events.hpp"
#include "src/core/net/protocol/remote_play/stream/stream.hpp"
#include "src/core/utils/diagnostics/trace.hpp"

#include <optional>
#include <span>
#include <string_view>

namespace libchiaking::play {

/// @brief Everything known about each link stage, from one constant table.
///
/// Stateless; every member reads the table.
class LinkStages {
 public:
  /// @brief A stage a link goes through.
  using Stage = net::protocol::remote_play::FailureStage;

  /// @brief Names the diagnostic operation of a stage.
  ///
  /// @param stage The stage.
  /// @return The operation.
  static auto operation(Stage stage) noexcept -> diagnostics::Operation;
  /// @brief Names the public end step of a stage.
  ///
  /// @param stage The stage.
  /// @return The step.
  static auto step(Stage stage) noexcept -> EndReason::Step;
  /// @brief Finds the stage of a public end step.
  ///
  /// @param step The step.
  /// @return The first stage with it; Stage::unknown when none.
  static auto stage(EndReason::Step step) noexcept -> Stage;
  /// @brief Names a stage for logs and messages.
  ///
  /// @param stage The stage.
  /// @return A short name such as "Console address lookup".
  static auto name(Stage stage) noexcept -> std::string_view;
  /// @brief Names the connect step the player sees for a stage.
  ///
  /// @param stage The stage.
  /// @return The step; empty for a stage the player does not see.
  static auto connect_step(Stage stage) noexcept -> std::optional<ConnectStep>;
  /// @brief Reports whether a stage belongs to a cloud launch.
  ///
  /// @param stage The stage.
  /// @return `true` for a launch stage.
  static auto launches(Stage stage) noexcept -> bool;
  /// @brief Names the internet pairing stage of a stage.
  ///
  /// @param stage The stage.
  /// @return The pairing stage; InternetPairingStage::starting when it has
  ///     none.
  static auto pairing_stage(Stage stage) noexcept
      -> consoles::InternetPairingStage;
  /// @brief Names the pairing step of a stage.
  ///
  /// @param stage The stage.
  /// @return The step; empty when it has none.
  static auto pairing_step(Stage stage) noexcept
      -> std::optional<consoles::PairingStep>;

 private:
  /// @brief One stage's entry.
  struct Row {
    /// @brief The stage.
    Stage stage;
    /// @brief Its diagnostic operation.
    diagnostics::Operation operation;
    /// @brief Its public end step.
    EndReason::Step step;
    /// @brief Its name.
    std::string_view name;
    /// @brief Its connect step, when the player sees one.
    std::optional<ConnectStep> connect{};
    /// @brief Whether it belongs to a cloud launch.
    bool launch{};
    /// @brief Its internet pairing stage, when it has one.
    std::optional<consoles::InternetPairingStage> pairing{};
    /// @brief Its pairing step, when it has one.
    std::optional<consoles::PairingStep> pairing_step{};
  };

  /// @brief The table, in stage order.
  ///
  /// @return The table.
  static auto rows() noexcept -> std::span<const Row>;
  /// @brief Finds a stage's entry by its value.
  ///
  /// @param stage The stage.
  /// @return The entry; the unknown stage's for a value past the table.
  static auto row(Stage stage) noexcept -> const Row&;
};

}  // namespace libchiaking::play
