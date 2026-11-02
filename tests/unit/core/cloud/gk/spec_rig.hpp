#pragma once

#include "src/core/net/protocol/takion/versions.hpp"
#include "src/core/psn/gk/launch_spec.hpp"
#include "tests/support/json/tree.hpp"
#include "tests/support/utc_offset.hpp"
#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace cloud_test {

class SpecRig {
 public:
  using JsonTree = test_support::JsonTree;
  using Tree = JsonTree::Tree;
  using Invalid = gk::LaunchSpec::Invalid;
  using Json = libchiaking::Expected<std::string, Invalid>;

  static constexpr auto cloud_stream_version =
      libchiaking::net::protocol::takion::takion_version_cloud_stream;
  static constexpr auto legacy_version =
      libchiaking::net::protocol::takion::takion_version_legacy_cloud_stream;

  static inline const test_support::FixedUtcOffset local_clock{
      std::chrono::minutes{-210}};

  static auto json_of(
      const libchiaking::Expected<gk::LaunchSpec, Invalid>& spec) -> Json {
    if (!spec) return libchiaking::unexpected(spec.error());
    return std::string{spec.value().json()};
  }

  static auto patch(CloudService service, std::string_view preauth,
                    std::string_view gk_code, std::string_view server)
      -> libchiaking::Expected<libchiaking::SecretString, Invalid> {
    return gk::LaunchSpec::prebuilt(service, std::string{preauth})
        .authorized(GkRig::codes(gk_code, server));
  }

  static auto object(std::string_view json) -> std::optional<Tree> {
    auto parsed = JsonTree::parse(json);
    if (!parsed || !parsed->is_object()) return std::nullopt;
    return parsed;
  }

  static auto has_keys(const Tree& object,
                       std::span<const std::string_view> keys)
      -> testing::AssertionResult {
    const auto& held = object.get<Tree::object_t>();
    if (held.size() != keys.size())
      return testing::AssertionFailure()
             << held.size() << " keys instead of " << keys.size();
    std::size_t index = 0;
    for (const auto& [name, value] : held) {
      if (name != keys[index])
        return testing::AssertionFailure() << "key " << index << " is " << name
                                           << " instead of " << keys[index];
      ++index;
    }
    return testing::AssertionSuccess();
  }

  static auto texts_of(const Tree* list)
      -> std::optional<std::vector<std::string>> {
    if (list == nullptr || !list->is_array()) return std::nullopt;
    std::vector<std::string> texts;
    for (const auto& item : list->get<Tree::array_t>())
      texts.emplace_back(JsonTree::text(item));
    return texts;
  }
};

}  // namespace cloud_test
