/// @file
/// @brief Declares SearchReply, a console's answer to a network search as
/// the consoles context trusts it: which console answered, its firmware,
/// name, address, whether it is awake and what it is running.
#pragma once

#include "libchiaking/detail/consoles/console.hpp"
#include "libchiaking/detail/consoles/console_name.hpp"
#include "libchiaking/detail/consoles/firmware.hpp"
#include "libchiaking/detail/consoles/hardware_id.hpp"
#include "libchiaking/platform/net/types.hpp"
#include "src/core/consoles/model/console/sighting.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace libchiaking::consoles {

/// @brief A screened search reply from a PS4 or PS5.
///
/// Made only through create(), so every SearchReply names a valid hardware
/// id and a firmware, its system version is printable and within
/// system_version_limit, and its running title has an id and printable texts. A
/// plain value: copyable, immutable, safe to share between threads.
class SearchReply {
 public:
  /// @brief The power state the reply's status line gave.
  enum class Status : std::uint8_t {
    /// @brief The status line was neither 200 nor 620.
    unknown,
    /// @brief Status 200: the console is switched on.
    ready,
    /// @brief Status 620: the console is in rest mode and can be woken.
    standby,
  };

  /// @brief Longest system version text a reply may carry, in bytes.
  static constexpr std::size_t system_version_limit = 63;

  /// @brief Makes a reply from judged fields, as SearchDatagram::from does.
  ///
  /// @param hardware_id The console's MAC address.
  /// @param firmware Its firmware.
  /// @param name Its name, if usable.
  /// @param address The sender.
  /// @param status The power state.
  /// @param request_port The request port, or zero.
  /// @param system_version The system version text.
  /// @param running_title The running title, if any.
  /// @return The reply, or nothing when the system version is longer than
  ///     system_version_limit or not printable, or the running title has no
  ///     id or a text that is not printable.
  [[nodiscard]] static auto create(HardwareId hardware_id, Firmware firmware,
                                   std::optional<ConsoleName> name,
                                   platform::IpAddress address, Status status,
                                   std::uint16_t request_port,
                                   std::string system_version,
                                   std::optional<RunningTitle> running_title)
      -> std::optional<SearchReply>;

  /// @brief The MAC address of the console that answered.
  ///
  /// @return The hardware id.
  auto hardware_id() const noexcept -> HardwareId { return hardware_id_; }
  /// @brief Whether a PS4 or a PS5 answered.
  ///
  /// @return The generation.
  auto generation() const noexcept -> Generation {
    return firmware_.generation();
  }
  /// @brief The firmware family the system version maps to.
  ///
  /// @return The firmware.
  auto firmware() const noexcept -> Firmware { return firmware_; }
  /// @brief The name the console stated.
  ///
  /// @return The name with ConsoleName::Source::discovered, or nothing when
  ///     it stated none usable.
  auto name() const noexcept -> const std::optional<ConsoleName>& {
    return name_;
  }
  /// @brief The IPv4 address the reply came from.
  ///
  /// @return The address.
  auto address() const noexcept -> platform::IpAddress { return address_; }
  /// @brief The power state from the status line.
  ///
  /// @return The status.
  auto status() const noexcept -> Status { return status_; }
  /// @brief Whether the console is in rest mode.
  ///
  /// @return True for Status::standby.
  auto in_standby() const noexcept -> bool {
    return status_ == Status::standby;
  }
  /// @brief The port the console said it takes requests on.
  ///
  /// @return The port, or zero when it gave none.
  auto request_port() const noexcept -> std::uint16_t { return request_port_; }
  /// @brief The system version text the console sent.
  ///
  /// @return The text, untrimmed.
  auto system_version() const noexcept -> std::string_view {
    return system_version_;
  }
  /// @brief The title the console said it is running.
  ///
  /// @return The title, or nothing.
  auto running_title() const noexcept -> const std::optional<RunningTitle>& {
    return running_title_;
  }
  /// @brief What the reply tells the roster about the console.
  ///
  /// @return The sighting, or nothing when the address cannot be made into a
  ///     Host for the console's generation.
  auto sighting() const -> std::optional<Sighting>;

  /// @brief Compares every field.
  friend auto operator==(const SearchReply&, const SearchReply&) noexcept
      -> bool = default;

 private:
  /// @brief Makes a reply from checked fields; see create().
  ///
  /// @param hardware_id The console's MAC address.
  /// @param firmware Its firmware.
  /// @param name Its name, if usable.
  /// @param address The sender.
  /// @param status The power state.
  /// @param request_port The request port, or zero.
  /// @param system_version The system version text.
  /// @param running_title The running title, if any.
  SearchReply(HardwareId hardware_id, Firmware firmware,
              std::optional<ConsoleName> name, platform::IpAddress address,
              Status status, std::uint16_t request_port,
              std::string system_version,
              std::optional<RunningTitle> running_title) noexcept;

  /// @brief The console's MAC address.
  HardwareId hardware_id_;
  /// @brief The firmware family.
  Firmware firmware_;
  /// @brief The stated name, if usable.
  std::optional<ConsoleName> name_;
  /// @brief The sender's IPv4 address.
  platform::IpAddress address_;
  /// @brief The power state.
  Status status_;
  /// @brief The request port; zero when none was given.
  std::uint16_t request_port_;
  /// @brief The system version text, as sent.
  std::string system_version_;
  /// @brief The running title, if any.
  std::optional<RunningTitle> running_title_;
};

}  // namespace libchiaking::consoles
