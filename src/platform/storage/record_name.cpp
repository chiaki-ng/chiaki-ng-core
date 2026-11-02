#include <libchiaking/platform/storage/record_store.hpp>

#include "src/core/utils/text/ascii.hpp"

#include <algorithm>
#include <array>

namespace libchiaking::platform {

auto RecordName::parse(std::string_view text) -> std::optional<RecordName> {
  if (text.empty() || text.size() > limit) return std::nullopt;
  for (auto rest = text;;) {
    const auto end = rest.find(separator);
    if (!valid_segment(rest.substr(0, end))) return std::nullopt;
    if (end == std::string_view::npos) break;
    rest.remove_prefix(end + 1);
  }
  return RecordName{text};
}

auto RecordName::is_under(const RecordName& prefix) const noexcept -> bool {
  const auto& outer = prefix.text_;
  if (text_ == outer) return true;
  return text_.size() > outer.size() && text_.starts_with(outer) &&
         text_[outer.size()] == separator;
}

auto RecordName::child(std::string_view segment) const
    -> std::optional<RecordName> {
  if (!valid_segment(segment)) return std::nullopt;
  return parse(text_ + separator + std::string{segment});
}

auto RecordName::valid_segment(std::string_view segment) noexcept -> bool {
  constexpr std::array<std::string_view, 4> devices{"con", "prn", "aux", "nul"};
  constexpr std::array<std::string_view, 2> numbered_devices{"com", "lpt"};
  constexpr std::size_t numbered_device_length = 4;
  const auto allowed = [](char value) {
    return text::ascii_lower_letter(value) || text::ascii_digit(value) ||
           value == '-' || value == '_';
  };
  if (segment.empty() || segment.size() > segment_limit ||
      !std::ranges::all_of(segment, allowed))
    return false;
  if (std::ranges::find(devices, segment) != devices.end()) return false;
  const bool numbered =
      segment.size() == numbered_device_length && segment.back() >= '0' &&
      segment.back() <= '9' &&
      std::ranges::find(numbered_devices,
                        segment.substr(0, numbered_device_length - 1)) !=
          numbered_devices.end();
  return !numbered;
}

auto RecordStore::is_legacy_file_name(std::string_view file_name) noexcept
    -> bool {
  constexpr std::string_view record_suffix = ".record";
  constexpr std::string_view separators = "/\\:";
  constexpr std::size_t longest = 96;
  return !file_name.empty() && file_name.size() <= longest &&
         file_name.front() != '.' &&
         file_name.find_first_of(separators) == std::string_view::npos &&
         file_name.find('\0') == std::string_view::npos &&
         !file_name.ends_with(record_suffix);
}

}  // namespace libchiaking::platform
