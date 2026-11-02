#include "src/core/play/adapters/links/link_stages.hpp"

#include <algorithm>
#include <array>
#include <ranges>
#include <utility>

namespace libchiaking::play {

auto LinkStages::rows() noexcept -> std::span<const Row> {
  using Op = diagnostics::Operation;
  using Step = EndReason::Step;
  using Connect = ConnectStep;
  using Pairing = consoles::InternetPairingStage;
  using Paired = consoles::PairingStep;
  static constexpr std::array table{
      Row{Stage::unknown, Op::unknown, Step::unknown, "Session"},
      Row{Stage::resolve, Op::resolve, Step::finding_console,
          "Console address lookup", Connect::finding_console},
      Row{Stage::registration, Op::registration, Step::pairing, "Registration",
          Connect::pairing, false, Pairing::pairing, Paired::sending_request},
      Row{Stage::session_init, Op::session_init, Step::session_request,
          "Session setup", Connect::requesting_session},
      Row{Stage::ctrl_startup, Op::ctrl_startup, Step::control_startup,
          "Ctrl startup response", Connect::opening_control},
      Row{Stage::ctrl_session_id, Op::ctrl_session_id, Step::control_session_id,
          "Ctrl session ID", Connect::opening_control},
      Row{Stage::ctrl_features, Op::ctrl_features, Step::control_features,
          "Ctrl feature setup", Connect::opening_control},
      Row{Stage::ctrl_receive, Op::ctrl_receive, Step::control_receive,
          "Ctrl receive", Connect::opening_control},
      Row{Stage::calibration, Op::calibration, Step::calibration,
          "Network calibration", Connect::measuring_connection},
      Row{Stage::route_switch, Op::route_switch, Step::route_switch,
          "Remote route switch", Connect::opening_stream_route},
      Row{Stage::active_connect, Op::active_connect, Step::stream_connect,
          "Stream transport connection", Connect::starting_stream},
      Row{Stage::active_bang, Op::active_bang, Step::stream_handshake,
          "Stream handshake (BANG)", Connect::starting_stream},
      Row{Stage::active_crypto, Op::active_crypto, Step::stream_crypto,
          "Stream encryption setup", Connect::starting_stream},
      Row{Stage::stream_info, Op::stream_info, Step::stream_description,
          "Stream format negotiation", Connect::starting_stream},
      Row{Stage::media, Op::media, Step::media, "Media stream"},
      Row{Stage::controller, Op::controller, Step::controller,
          "Controller delivery"},
      Row{Stage::psn_open, Op::psn_open, Step::account_session_open,
          "PSN session creation", Connect::opening_account_session, false,
          Pairing::opening_session, Paired::opening_account_session},
      Row{Stage::psn_start, Op::psn_start, Step::account_session_start,
          "PSN console readiness", Connect::opening_account_session, false,
          Pairing::waking_console, Paired::waking_console},
      Row{Stage::control_holepunch, Op::control_holepunch,
          Step::control_holepunch, "Control route hole punching",
          Connect::reaching_console, false, Pairing::connecting,
          Paired::reaching_console},
      Row{Stage::data_holepunch, Op::data_holepunch, Step::data_holepunch,
          "Data route hole punching", Connect::opening_stream_route, false,
          Pairing::connecting, Paired::reaching_console},
      Row{Stage::cloud_provision, Op::cloud_provision, Step::cloud_launch,
          "Cloud provisioning", std::nullopt, true},
      Row{Stage::active_prepare, Op::active_prepare, Step::stream_prepare,
          "Stream preparation", Connect::starting_stream},
      Row{Stage::shutdown, Op::shutdown, Step::shutdown, "Session cleanup"},
      Row{Stage::cloud_identity, Op::cloud_identity, Step::cloud_device_check,
          "Cloud account authorization", std::nullopt, true},
      Row{Stage::cloud_client_ids, Op::cloud_client_ids, Step::cloud_client_ids,
          "Cloud client identification", std::nullopt, true},
      Row{Stage::cloud_config, Op::cloud_config, Step::cloud_config,
          "Cloud configuration", std::nullopt, true},
      Row{Stage::cloud_start, Op::cloud_start, Step::cloud_start,
          "Cloud session creation", std::nullopt, true},
      Row{Stage::cloud_oauth, Op::cloud_oauth, Step::cloud_codes,
          "Cloud streaming authorization", std::nullopt, true},
      Row{Stage::cloud_authorize, Op::cloud_authorize, Step::cloud_authorise,
          "Cloud entitlement authorization", std::nullopt, true},
      Row{Stage::cloud_lock, Op::cloud_lock, Step::cloud_lock,
          "Cloud session lock", std::nullopt, true},
      Row{Stage::cloud_datacenter, Op::cloud_datacenter, Step::cloud_datacenter,
          "Cloud datacenter selection", std::nullopt, true},
      Row{Stage::cloud_allocate, Op::cloud_allocate, Step::cloud_allocate,
          "Cloud streaming slot allocation", std::nullopt, true},
      Row{Stage::cloud_title, Op::cloud_title, Step::cloud_title,
          "LegacyCloudStream title preparation", std::nullopt, true},
      Row{Stage::cloud_launch_check, Op::cloud_launch_check,
          Step::cloud_clearance, "Cloud launch checks", std::nullopt, true},
  };
  static_assert(std::ranges::all_of(
      std::views::iota(std::size_t{0}, table.size()), [](std::size_t index) {
        return std::to_underlying(table[index].stage) == index;
      }));
  return table;
}

auto LinkStages::row(Stage stage) noexcept -> const Row& {
  const auto table = rows();
  const auto index = static_cast<std::size_t>(std::to_underlying(stage));
  return index < table.size() ? table[index] : table.front();
}

auto LinkStages::operation(Stage stage) noexcept -> diagnostics::Operation {
  return row(stage).operation;
}

auto LinkStages::step(Stage stage) noexcept -> EndReason::Step {
  return row(stage).step;
}

auto LinkStages::stage(EndReason::Step step) noexcept -> Stage {
  const auto table = rows();
  const auto found = std::ranges::find(table, step, &Row::step);
  return found != table.end() ? found->stage : Stage::unknown;
}

auto LinkStages::name(Stage stage) noexcept -> std::string_view {
  return row(stage).name;
}

auto LinkStages::connect_step(Stage stage) noexcept
    -> std::optional<ConnectStep> {
  return row(stage).connect;
}

auto LinkStages::launches(Stage stage) noexcept -> bool {
  return row(stage).launch;
}

auto LinkStages::pairing_stage(Stage stage) noexcept
    -> consoles::InternetPairingStage {
  return row(stage).pairing.value_or(consoles::InternetPairingStage::starting);
}

auto LinkStages::pairing_step(Stage stage) noexcept
    -> std::optional<consoles::PairingStep> {
  return row(stage).pairing_step;
}

}  // namespace libchiaking::play
