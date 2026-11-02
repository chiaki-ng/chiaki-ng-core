#include "libchiaking/version.hpp"

#include "version_config.hpp"

#include <format>

namespace libchiaking {

auto Version::current() noexcept -> Version {
  return Version{LIBCHIAKING_VERSION_MAJOR, LIBCHIAKING_VERSION_MINOR,
                 LIBCHIAKING_VERSION_PATCH, LIBCHIAKING_VERSION_PRERELEASE,
                 LIBCHIAKING_COMMIT};
}

auto Version::text() const -> std::string {
  auto formatted = std::format("{}.{}.{}", major_, minor_, patch_);
  if (!prerelease_.empty()) formatted += std::format("-{}", prerelease_);
  if (!commit_.empty()) formatted += std::format("+{}", commit_);
  return formatted;
}

}  // namespace libchiaking
