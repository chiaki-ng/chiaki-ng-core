#include "src/core/consoles/model/search/search_reply.hpp"

#include "libchiaking/detail/consoles/host.hpp"

#include <utility>

namespace libchiaking::consoles {

auto SearchReply::create(HardwareId hardware_id, Firmware firmware,
                         std::optional<ConsoleName> name,
                         platform::IpAddress address, Status status,
                         std::uint16_t request_port, std::string system_version,
                         std::optional<RunningTitle> running_title)
    -> std::optional<SearchReply> {
  if (system_version.size() > system_version_limit ||
      !ConsoleName::printable(system_version))
    return std::nullopt;
  if (running_title && (running_title->title_id.empty() ||
                        !ConsoleName::printable(running_title->title_id) ||
                        !ConsoleName::printable(running_title->name)))
    return std::nullopt;
  return SearchReply{hardware_id,
                     firmware,
                     std::move(name),
                     address,
                     status,
                     request_port,
                     std::move(system_version),
                     std::move(running_title)};
}

SearchReply::SearchReply(HardwareId hardware_id, Firmware firmware,
                         std::optional<ConsoleName> name,
                         platform::IpAddress address, Status status,
                         std::uint16_t request_port, std::string system_version,
                         std::optional<RunningTitle> running_title) noexcept
    : hardware_id_(hardware_id),
      firmware_(firmware),
      name_(std::move(name)),
      address_(address),
      status_(status),
      request_port_(request_port),
      system_version_(std::move(system_version)),
      running_title_(std::move(running_title)) {}

auto SearchReply::sighting() const -> std::optional<Sighting> {
  auto address = Host::create(platform::to_string(address_), generation());
  if (!address) return std::nullopt;
  return Sighting{hardware_id_, firmware_,
                  name_,        std::move(address.value()),
                  in_standby(), running_title_};
}

}  // namespace libchiaking::consoles
