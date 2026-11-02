#include "src/platform/net/posix_dont_fragment.hpp"
#include "tests/support/enum_printing.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <initializer_list>

namespace {

using libchiaking::platform::NetError;
using libchiaking::platform::detail::apply_dont_fragment;
using libchiaking::platform::detail::dont_fragment_available;

class DontFragment : public ::testing::Test {
 protected:
  struct RecordedCall {
    int calls{0};
    int level{-1};
    int name{-1};
  };

  [[nodiscard]] static auto apply_failing(int level, bool enabled,
                                          int error_number,
                                          RecordedCall& recorded) {
    return apply_dont_fragment(
        level, enabled,
        [&recorded, error_number](int option_level, int name, const void*,
                                  socklen_t) {
          ++recorded.calls;
          recorded.level = option_level;
          recorded.name = name;
          errno = error_number;
          return -1;
        },
        [] { return NetError::system_failure; });
  }

  static constexpr bool option_known =
#if defined(IP_DONTFRAGMENT) || defined(IP_DONTFRAG) || defined(IP_MTU_DISCOVER)
      true;
#else
      false;
#endif

  static constexpr std::initializer_list<int> missing_errors{
      ENOPROTOOPT, EINVAL, EOPNOTSUPP};
  static constexpr std::initializer_list<int> levels{
      static_cast<int>(IPPROTO_IP), static_cast<int>(IPPROTO_IPV6)};
};

TEST_F(DontFragment, AMissingOptionReportsUnsupportedWhenEnabling) {
  for (const int error_number : missing_errors) {
    for (const int level : levels) {
      RecordedCall recorded;
      const auto result = apply_failing(level, true, error_number, recorded);
      EXPECT_LE(recorded.calls, 1)
          << "errno " << error_number << " level " << level;
      ASSERT_FALSE(result) << "errno " << error_number << " level " << level;
      EXPECT_EQ(result.error(), NetError::not_supported)
          << "errno " << error_number << " level " << level;
    }
  }
}

TEST_F(DontFragment, AMissingOptionIsSilentWhenDisabling) {
  for (const int error_number : missing_errors) {
    for (const int level : levels) {
      RecordedCall recorded;
      EXPECT_TRUE(apply_failing(level, false, error_number, recorded))
          << "errno " << error_number << " level " << level;
    }
  }
}

TEST_F(DontFragment, OtherFailuresStillSurface) {
  RecordedCall recorded;
  const auto enabled = apply_failing(IPPROTO_IP, true, EBADF, recorded);
  ASSERT_FALSE(enabled);
  if constexpr (option_known) {
    EXPECT_EQ(enabled.error(), NetError::system_failure);
    const auto disabled = apply_failing(IPPROTO_IP, false, EBADF, recorded);
    ASSERT_FALSE(disabled);
    EXPECT_EQ(disabled.error(), NetError::system_failure);
  } else {
    EXPECT_EQ(enabled.error(), NetError::not_supported);
  }
}

TEST_F(DontFragment, ASuccessfulOptionApplies) {
  RecordedCall recorded;
  const auto result = apply_dont_fragment(
      IPPROTO_IP, true,
      [&recorded](int option_level, int name, const void*, socklen_t) {
        ++recorded.calls;
        recorded.level = option_level;
        recorded.name = name;
        return 0;
      },
      [] { return NetError::system_failure; });
  if constexpr (option_known) {
    EXPECT_TRUE(result);
    EXPECT_EQ(recorded.calls, 1);
    EXPECT_EQ(recorded.level, IPPROTO_IP);
  } else {
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error(), NetError::not_supported);
  }
}

TEST_F(DontFragment, TheCapabilityFollowsTheRunningKernel) {
  const auto missing = [](int, int, const void*, socklen_t) {
    errno = ENOPROTOOPT;
    return -1;
  };
  EXPECT_FALSE(dont_fragment_available(IPPROTO_IP, missing));
  EXPECT_FALSE(dont_fragment_available(IPPROTO_IPV6, missing));
  const auto present = [](int, int, const void*, socklen_t) { return 0; };
  EXPECT_EQ(dont_fragment_available(IPPROTO_IP, present), option_known);
}

}  // namespace
