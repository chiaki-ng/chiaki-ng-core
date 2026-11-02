#include "tests/unit/core/cloud/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace cloud_test {
namespace {

class GkSessionKeyHeader : public ::testing::Test {
 protected:
  using Headers = std::vector<std::pair<std::string, std::string>>;

  static auto lock(Headers headers)
      -> libchiaking::Expected<gk::LockAttempt, gk::Error> {
    RecordingHttp http;
    http.reply_headers = std::move(headers);
    Gk client{http};
    return client.lock(GkRig::session_id("synthetic-session"),
                       GkRig::config_key("synthetic-key"), "{}");
  }

  static auto rotates_to(Headers headers, std::string_view expected)
      -> testing::AssertionResult {
    const auto result = lock(std::move(headers));
    if (!result) return testing::AssertionFailure() << "the lock failed";
    if (!result.value().rotated_key)
      return testing::AssertionFailure() << "the key was not rotated";
    if (result.value().rotated_key->view() != expected)
      return testing::AssertionFailure()
             << "rotated to " << result.value().rotated_key->view()
             << " instead of " << expected;
    return testing::AssertionSuccess();
  }

  static auto keeps_the_key(Headers headers) -> testing::AssertionResult {
    const auto result = lock(std::move(headers));
    if (!result) return testing::AssertionFailure() << "the lock failed";
    if (result.value().rotated_key)
      return testing::AssertionFailure()
             << "rotated to " << result.value().rotated_key->view();
    return testing::AssertionSuccess();
  }

  static auto refuses(std::string invalid) -> testing::AssertionResult {
    const auto result = lock({{"x-gaikai-session", invalid}});
    if (result) return testing::AssertionFailure() << "the lock succeeded";
    if (result.error().reason() != gk::Error::Reason::invalid_session_key)
      return testing::AssertionFailure()
             << "failed for another reason than an invalid session key";
    return testing::AssertionSuccess();
  }
};

TEST_F(GkSessionKeyHeader, RotatesTheKeyFromTheHeaderInAnyCase) {
  EXPECT_TRUE(rotates_to({{"X-Gaikai-Session", "abcdef"}}, "abcdef"));
}

TEST_F(GkSessionKeyHeader, TrimsBlanksAroundTheRotatedKey) {
  EXPECT_TRUE(rotates_to({{"x-gaikai-session", " \tabc\t "}}, "abc"));
}

TEST_F(GkSessionKeyHeader, TrimsOnlyBlanksAndTabsAroundTheRotatedKey) {
  EXPECT_TRUE(rotates_to(
      {{std::string{libchiaking::psn::gk::ConfigKeyKind::placement.header},
        " \vabc\f\t"}},
      "\vabc\f"));
}

TEST_F(GkSessionKeyHeader, KeepsTheKeyWithoutAHeader) {
  EXPECT_TRUE(keeps_the_key({{"content-type", "application/json"}}));
}

TEST_F(GkSessionKeyHeader, KeepsTheKeyForABlankHeader) {
  EXPECT_TRUE(keeps_the_key({{"x-gaikai-session", ""}}));
  EXPECT_TRUE(keeps_the_key({{"X-GAIKAI-SESSION", " \t "}}));
}

TEST_F(GkSessionKeyHeader, TakesTheFirstOfRepeatedHeaders) {
  EXPECT_TRUE(rotates_to(
      {{"x-gaikai-session", "first"}, {"X-GAIKAI-SESSION", "second"}},
      "first"));
}

TEST_F(GkSessionKeyHeader, AcceptsControlLongAndHighBitKeys) {
  for (const std::string& accepted :
       {std::string{"ab\x01"
                    "cd"},
        std::string(4097, 'k'), std::string{"key with\x80space"}})
    EXPECT_TRUE(rotates_to({{"x-gaikai-session", accepted}}, accepted));
}

TEST_F(GkSessionKeyHeader, RefusesKeysThatBreakAHeader) {
  EXPECT_TRUE(refuses(std::string{"abc\r\nX: y"}));
  EXPECT_TRUE(refuses(std::string{"ab\0cd", 5}));
  EXPECT_TRUE(refuses(std::string{"a\nb"}));
}

class GkKeyValidation : public ::testing::Test {
 protected:
  static auto accepts_config_key(std::string_view key)
      -> testing::AssertionResult {
    RecordingHttp http;
    http.reply_body = R"({"sessionId":"synthetic-session"})";
    Gk client{http};
    const auto result = client.start("{}", GkRig::config_key(key));
    if (!result) return testing::AssertionFailure() << "the key was refused";
    if (http.calls != 1)
      return testing::AssertionFailure() << http.calls << " requests were sent";
    if (http.last.headers.back().value.view() != key)
      return testing::AssertionFailure() << "the key was not sent as it was";
    return testing::AssertionSuccess();
  }

  static auto refuses_config_key(std::string_view key)
      -> testing::AssertionResult {
    RecordingHttp http;
    http.reply_body = R"({"sessionId":"synthetic-session"})";
    Gk client{http};
    const auto result = client.start("{}", GkRig::config_key(key));
    if (result) return testing::AssertionFailure() << "the key was accepted";
    if (result.error().reason() != gk::Error::Reason::invalid_config_key)
      return testing::AssertionFailure()
             << "failed for another reason than an invalid config key";
    if (http.calls != 0)
      return testing::AssertionFailure() << http.calls << " requests were sent";
    return testing::AssertionSuccess();
  }

  static auto accepts_session_id(std::string_view id)
      -> testing::AssertionResult {
    RecordingHttp http;
    Gk client{http};
    const auto result = client.lock(GkRig::session_id(id),
                                    GkRig::config_key("synthetic-key"), "{}");
    if (!result) return testing::AssertionFailure() << "the id was refused";
    if (http.calls != 1)
      return testing::AssertionFailure() << http.calls << " requests were sent";
    return testing::AssertionSuccess();
  }

  static auto refuses_session_id(std::string_view id)
      -> testing::AssertionResult {
    RecordingHttp http;
    Gk client{http};
    const auto result = client.lock(GkRig::session_id(id),
                                    GkRig::config_key("synthetic-key"), "{}");
    if (result) return testing::AssertionFailure() << "the id was accepted";
    if (result.error().reason() != gk::Error::Reason::invalid_session_id)
      return testing::AssertionFailure()
             << "failed for another reason than an invalid session id";
    if (http.calls != 0)
      return testing::AssertionFailure() << http.calls << " requests were sent";
    return testing::AssertionSuccess();
  }
};

TEST_F(GkKeyValidation, AcceptsAnyConfigKeyThatFitsAHeader) {
  EXPECT_TRUE(accepts_config_key("a"));
  EXPECT_TRUE(accepts_config_key("!"));
  EXPECT_TRUE(accepts_config_key("~"));
  EXPECT_TRUE(accepts_config_key("0123-ABC_zy."));
  EXPECT_TRUE(accepts_config_key(std::string(4096, 'x')));
  EXPECT_TRUE(accepts_config_key(std::string(4097, 'x')));
  EXPECT_TRUE(accepts_config_key("a b"));
  EXPECT_TRUE(accepts_config_key("\x80"));
  EXPECT_TRUE(accepts_config_key("\xffz"));
  EXPECT_TRUE(
      accepts_config_key(std::string_view{"a\x1f"
                                          "b",
                                          3}));
}

TEST_F(GkKeyValidation, RefusesAnEmptyConfigKeyOrOneThatBreaksAHeader) {
  EXPECT_TRUE(refuses_config_key(""));
  EXPECT_TRUE(refuses_config_key("a\r\nb"));
  EXPECT_TRUE(refuses_config_key("a\nb"));
  EXPECT_TRUE(refuses_config_key("a\rb"));
  EXPECT_TRUE(refuses_config_key(std::string_view{"a\0b", 3}));
  EXPECT_TRUE(refuses_config_key(std::string_view{"\0", 1}));
}

TEST_F(GkKeyValidation, AcceptsASessionIdWithSlashesAndSpaces) {
  EXPECT_TRUE(accepts_session_id("a/b c"));
}

TEST_F(GkKeyValidation, RefusesAnEmptySessionIdOrOneThatBreaksAPath) {
  EXPECT_TRUE(refuses_session_id(""));
  EXPECT_TRUE(refuses_session_id("a\nb"));
}

}  // namespace
}  // namespace cloud_test
