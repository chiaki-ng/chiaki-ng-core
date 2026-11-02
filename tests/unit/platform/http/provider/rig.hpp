#pragma once

#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/http/provider.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace platform_http_test {

class ScriptedHttpProvider final : public libchiaking::platform::HttpProvider {
 public:
  ScriptedHttpProvider(libchiaking::platform::HttpResponse response,
                       std::optional<libchiaking::platform::HttpError> error)
      : response_(std::move(response)), error_(error) {}

  auto perform(const libchiaking::platform::HttpRequest& request,
               const libchiaking::platform::StopToken&)
      -> libchiaking::Expected<libchiaking::platform::HttpResponse,
                               libchiaking::platform::HttpError> override {
    captured_request_ = request.clone();
    ++performed_;
    if (error_.has_value()) return libchiaking::unexpected(*error_);
    return response_.clone();
  }

  [[nodiscard]] auto captured_request() const noexcept
      -> const libchiaking::platform::HttpRequest& {
    return captured_request_;
  }

  [[nodiscard]] auto performed() const noexcept -> int { return performed_; }

 private:
  libchiaking::platform::HttpResponse response_;
  std::optional<libchiaking::platform::HttpError> error_;
  libchiaking::platform::HttpRequest captured_request_;
  int performed_ = 0;
};

class HttpValuesTest : public ::testing::Test {
 protected:
  [[nodiscard]] static auto bytes(std::initializer_list<unsigned> values)
      -> std::vector<std::byte> {
    std::vector<std::byte> output;
    for (const auto value : values)
      output.push_back(static_cast<std::byte>(value));
    return output;
  }

  [[nodiscard]] static auto same_bytes(std::span<const std::byte> value,
                                       std::span<const std::byte> expected)
      -> ::testing::AssertionResult {
    if (std::ranges::equal(value, expected))
      return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << value.size() << " bytes differ from the " << expected.size()
           << " expected";
  }

  [[nodiscard]] static auto same_headers(
      const std::vector<libchiaking::platform::HttpHeader>& value,
      const std::vector<libchiaking::platform::HttpHeader>& expected)
      -> ::testing::AssertionResult {
    if (value.size() != expected.size())
      return ::testing::AssertionFailure()
             << value.size() << " headers rather than " << expected.size();
    for (std::size_t index = 0; index < value.size(); ++index) {
      if (value[index].name != expected[index].name ||
          value[index].value.view() != expected[index].value.view())
        return ::testing::AssertionFailure()
               << "header " << index << " is " << value[index].name
               << " rather than " << expected[index].name;
    }
    return ::testing::AssertionSuccess();
  }
};

}  // namespace platform_http_test
