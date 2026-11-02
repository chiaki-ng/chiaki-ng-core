#include "src/core/consoles/adapters/search/probe_diagnostics.hpp"

#include <algorithm>
#include <format>
#include <limits>
#include <span>

namespace libchiaking::consoles {

void ProbeDiagnostics::increment(std::uint64_t& value) noexcept {
  if (value != std::numeric_limits<std::uint64_t>::max()) ++value;
}

auto ProbeDiagnostics::reason(Beacon::ProbeFailure error) noexcept
    -> std::string_view {
  switch (error) {
    case Beacon::ProbeFailure::permission_denied:
      return "permission denied";
    case Beacon::ProbeFailure::short_send:
      return "short send";
    case Beacon::ProbeFailure::not_started:
    case Beacon::ProbeFailure::address_family_mismatch:
    case Beacon::ProbeFailure::network:
      break;
  }
  return "network failure";
}

void ProbeDiagnostics::write(platform::LogSeverity severity,
                             std::string_view message) const noexcept {
  if (output_ != nullptr) output_->write({severity, message});
}

void ProbeDiagnostics::start(std::uint64_t now_us, std::size_t interfaces,
                             std::size_t targets, std::uint16_t ps4_port,
                             std::uint16_t ps5_port) {
  has_started_ = true;
  if (output_ == nullptr) return;
  started_ = timing::Deadline::instant_of(now_us);
  next_silence_ = timing::Deadline::after(started_, first_silence);
  write(platform::LogSeverity::info,
        std::format("Scanning {} IPv4 interfaces; {} unicast targets; PS4 UDP "
                    "{}, PS5 UDP {}; "
                    "broadcast every 1 s, unicast batches of 64 every 200 ms "
                    "then every 15 s",
                    interfaces, targets, ps4_port, ps5_port));
}

void ProbeDiagnostics::sent(platform::SocketEndpoint destination,
                            std::optional<Beacon::ProbeFailure> error) {
  if (output_ == nullptr) return;
  increment(probes_);
  if (!error) return;
  increment(failures_);
  const auto known = std::span{reports_}.first(reported_);
  const auto repeated = [&](const Report& report) {
    return report.destination == destination && report.error == *error;
  };
  if (std::ranges::any_of(known, repeated)) return;
  if (reported_ == reports_.size()) {
    if (!suppressed_) {
      suppressed_ = true;
      write(platform::LogSeverity::warning,
            "Further distinct probe errors are counted in silence summaries");
    }
    return;
  }
  reports_[reported_++] = {destination, *error};
  write(platform::LogSeverity::warning,
        std::format("Probe to {}:{} failed: {}",
                    platform::to_string(destination.address), destination.port,
                    reason(*error)));
}

void ProbeDiagnostics::received(bool parsed) noexcept {
  if (output_ == nullptr) return;
  increment(replies_);
  if (!parsed) increment(unparsed_);
}

void ProbeDiagnostics::silence(std::uint64_t now_us, std::size_t found) {
  if (!silence_due(now_us)) return;
  const auto now = timing::Deadline::instant_of(now_us);
  const auto elapsed = timing::elapsed(started_, now);
  next_silence_ = elapsed < second_silence
                      ? timing::Deadline::after(started_, second_silence)
                      : timing::Deadline::after(now, repeated_silence);
  if (found != 0) return;
  write(platform::LogSeverity::warning,
        std::format(
            "No console has answered after {} s; sent {} probes; {} failed "
            "sends; "
            "received {} packets, {} unparsed",
            std::chrono::duration_cast<std::chrono::seconds>(elapsed).count(),
            probes_, failures_, replies_, unparsed_));
}

}  // namespace libchiaking::consoles
