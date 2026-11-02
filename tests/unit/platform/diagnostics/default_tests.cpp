#include "tests/support/enum_printing.hpp"

#include <libchiaking/platform/default_factory.hpp>
#include <libchiaking/platform/diagnostics/default.hpp>
#include <libchiaking/platform/factory.hpp>

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

using namespace libchiaking::platform;

class DefaultDiagnostics : public ::testing::Test {
 protected:
  DefaultDiagnosticsProvider provider{"org.chiaki.tests", "diagnostics"};
};

TEST_F(DefaultDiagnostics, WritesEverySeverityAndAnEmbeddedNul) {
  constexpr std::array severities{
      LogSeverity::error,   LogSeverity::warning, LogSeverity::info,
      LogSeverity::verbose, LogSeverity::debug,
  };
  for (const auto severity : severities)
    provider.write(LogRecord{severity, std::string_view{"prefix\0suffix", 13}});
  SUCCEED();
}

TEST_F(DefaultDiagnostics, IsOfferedByAFactoryItIsGivenTo) {
  DefaultPlatformFactory factory{PlatformProviders{.diagnostics = &provider}};
  EXPECT_EQ(&factory.diagnostics(), &provider);
  EXPECT_EQ(factory.info().capabilities & PlatformCapability::diagnostics,
            PlatformCapability::diagnostics);
}

}  // namespace
