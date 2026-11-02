#include "tests/support/enum_printing.hpp"
#include "tests/unit/core/utils/diagnostics/rig.hpp"

#include <gtest/gtest.h>

namespace diagnostics_test {
namespace {

using libchiaking::SecretString;
using libchiaking::diagnostics::HttpMarkedText;
using libchiaking::diagnostics::HttpSecretKind;
using libchiaking::diagnostics::HttpSecretSpan;

TEST(HttpMarkedSpans, SortsClipsAndMergesSpans) {
  const HttpMarkedText marked{SecretString{"0123456789"},
                              {{6, 10, HttpSecretKind::field},
                               {0, 2, HttpSecretKind::cookie},
                               {1, 3, HttpSecretKind::field},
                               {4, 0, HttpSecretKind::field}}};
  ASSERT_EQ(marked.spans().size(), 2U);
  EXPECT_EQ(marked.spans()[0], (HttpSecretSpan{0, 4, HttpSecretKind::cookie}));
  EXPECT_EQ(marked.spans()[1], (HttpSecretSpan{6, 4, HttpSecretKind::field}));
  const auto copy = marked.clone();
  EXPECT_EQ(copy.text(), marked.text());
  EXPECT_EQ(copy.spans().size(), 2U);
}

}  // namespace
}  // namespace diagnostics_test
