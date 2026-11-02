#include "tests/unit/core/psn/auth/rig.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <utility>

namespace psn_test {
namespace {

using libchiaking::psn::auth::AuthErrorKind;
using Reply = ClientRig::Reply;

class RedirectCredential : public ::testing::Test {
 protected:
  static auto coded(std::string location) -> std::string {
    AuthRig fresh;
    const auto result = fresh.code(AuthRig::redirect(std::move(location)));
    return result ? std::string{result.value().view()} : std::string{};
  }

  static constexpr std::string_view redirect_uri =
      "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
      "grc-response.html";

  static auto at_redirect(std::string_view tail) -> std::string {
    return std::string{redirect_uri} + std::string{tail};
  }

  AuthRig rig;
};

TEST_F(RedirectCredential, MustBeTheOneCodeOfTheRedirectQuery) {
  EXPECT_EQ(coded(at_redirect("?xcode=bad&code=good")), "good");
  EXPECT_TRUE(coded("code=first").empty());
  EXPECT_TRUE(coded(at_redirect("?code=a#frag")).empty());
  EXPECT_EQ(coded(at_redirect(std::string_view{"?code=ab\0cd", 11})), "ab");
  EXPECT_TRUE(coded(at_redirect("?other=1")).empty());
  EXPECT_TRUE(coded(at_redirect("?mycode=abc")).empty());
  EXPECT_TRUE(coded(at_redirect("?codec=abc")).empty());
  EXPECT_TRUE(coded(at_redirect("/code=x")).empty());
  EXPECT_EQ(coded(at_redirect("?code=a%2Fb%3D")), "a/b=");
  EXPECT_TRUE(coded(at_redirect("?code=first&code=second")).empty());
  EXPECT_TRUE(coded(at_redirect("?code=line\r\nnext")).empty());
}

TEST_F(RedirectCredential, MayFollowAnotherFragmentParameter) {
  const auto fragment =
      rig.token(AuthRig::redirect("https://x/#state=1&access_token=T"));
  ASSERT_TRUE(fragment);
  EXPECT_EQ(fragment.value().view(), "T");
}

TEST_F(RedirectCredential,
       IsRefusedRatherThanCutWhenATokenIsLongerThan2047Bytes) {
  const auto longest = rig.token(
      AuthRig::redirect("https://x/#access_token=" + std::string(2047, 't')));
  ASSERT_TRUE(longest);
  EXPECT_EQ(longest.value().view().size(), 2047U);
  AuthRig over;
  EXPECT_EQ(AuthRig::kind_of(over.token(AuthRig::redirect(
                "https://x/#access_token=" + std::string(2048, 't')))),
            AuthErrorKind::malformed);
  AuthRig far_over;
  EXPECT_EQ(AuthRig::kind_of(far_over.token(AuthRig::redirect(
                "https://x/#access_token=" + std::string(3000, 't')))),
            AuthErrorKind::malformed);
}

TEST_F(RedirectCredential, IsReadFromATrimmedLocationOfAnyCase) {
  Reply spaced{302, ""};
  spaced.headers.push_back(
      {"lOcAtIoN", " \t" + at_redirect("?code=trimmed") + "\t "});
  const auto result = rig.code(spaced);
  ASSERT_TRUE(result);
  EXPECT_EQ(result.value().view(), "trimmed");
}

TEST_F(RedirectCredential, IsRefusedWhenTheLocationEndsInAnotherControl) {
  Reply tabbed{302, ""};
  tabbed.headers.push_back({"Location", "https://x/#access_token=T\t"});
  const auto trimmed = rig.token(tabbed);
  ASSERT_TRUE(trimmed);
  EXPECT_EQ(trimmed.value().view(), "T");
  for (const std::string_view end : {"\v", "\f"}) {
    AuthRig fresh;
    Reply padded{302, ""};
    padded.headers.push_back(
        {"Location", "https://x/#access_token=T" + std::string{end}});
    EXPECT_EQ(AuthRig::kind_of(fresh.token(padded)), AuthErrorKind::malformed);
  }
}

TEST_F(RedirectCredential, IsReadOnlyFromTheFirstLocation) {
  Reply twice{302, ""};
  twice.headers.push_back({"Location", at_redirect("?state=x")});
  twice.headers.push_back({"Location", at_redirect("?code=second")});
  const auto missing = rig.code(twice);
  ASSERT_FALSE(missing);
  EXPECT_EQ(missing.error().kind(), AuthErrorKind::malformed);
  EXPECT_TRUE(missing.error().denied());
}

TEST_F(RedirectCredential, IsReadFromALocationOf16KiB) {
  const auto prefix = at_redirect("?code=C&pad=");
  const auto bounded = rig.code(
      AuthRig::redirect(prefix + std::string(16 * 1024 - prefix.size(), 'x')));
  ASSERT_TRUE(bounded);
  EXPECT_EQ(bounded.value().view(), "C");
}

TEST_F(RedirectCredential, IsRefusedRatherThanCutWhenLongerThan1023Bytes) {
  const auto longest = rig.code(
      AuthRig::redirect(at_redirect("?code=" + std::string(1023, 'c'))));
  ASSERT_TRUE(longest);
  EXPECT_EQ(longest.value().view().size(), 1023U);
  AuthRig over;
  EXPECT_EQ(AuthRig::kind_of(over.code(AuthRig::redirect(
                at_redirect("?code=" + std::string(1024, 'c'))))),
            AuthErrorKind::malformed);
}

TEST_F(RedirectCredential, IsRefusedFromALocationOver16KiB) {
  const auto too_large =
      rig.code(AuthRig::redirect(std::string(16 * 1024 + 1, 'x')));
  ASSERT_FALSE(too_large);
  EXPECT_EQ(too_large.error().kind(), AuthErrorKind::malformed);
  EXPECT_EQ(too_large.error().http_status(), 302);
}

TEST_F(RedirectCredential, EndsAtANulBeforeAnOversizedTail) {
  const auto nul =
      rig.code(AuthRig::redirect(at_redirect(std::string_view{"?code=z\0", 8}) +
                                 std::string(20 * 1024, 'x')));
  ASSERT_TRUE(nul);
  EXPECT_EQ(nul.value().view(), "z");
}

TEST_F(RedirectCredential, IsRefusedBesideAnErrorParameter) {
  const auto result = rig.token(
      AuthRig::redirect("https://x/?error=login_required#access_token=T"));
  ASSERT_FALSE(result);
  EXPECT_EQ(result.error().kind(), AuthErrorKind::refused);
  EXPECT_TRUE(result.error().denied());
}

TEST_F(RedirectCredential, IsRefusedWhenTheWantedKindIsMissing) {
  EXPECT_EQ(AuthRig::kind_of(rig.token(AuthRig::redirect("https://x/?code=C"))),
            AuthErrorKind::malformed);
  AuthRig code;
  const auto token_for_code =
      code.code(AuthRig::redirect("https://x/#access_token=T"));
  ASSERT_FALSE(token_for_code);
  EXPECT_TRUE(token_for_code.error().denied());
  AuthRig empty;
  const auto empty_token =
      empty.token(AuthRig::redirect("https://x/#access_token=&x=1"));
  ASSERT_FALSE(empty_token);
  EXPECT_TRUE(empty_token.error().denied());
  AuthRig empty_code;
  EXPECT_EQ(AuthRig::kind_of(
                empty_code.code(AuthRig::redirect("gaikai://local?code="))),
            AuthErrorKind::malformed);
  AuthRig unprintable;
  const auto spaced =
      unprintable.token(AuthRig::redirect("https://x/#access_token=a b"));
  ASSERT_FALSE(spaced);
  EXPECT_TRUE(spaced.error().denied());
}

TEST_F(RedirectCredential, IsReadOnlyFromTheRequestedRedirectAddress) {
  AuthRig expected;
  const auto read = expected.code(AuthRig::redirect(at_redirect("?code=C")));
  ASSERT_TRUE(read);
  EXPECT_EQ(read.value().view(), "C");
  AuthRig elsewhere;
  EXPECT_EQ(AuthRig::kind_of(elsewhere.code(
                AuthRig::redirect("https://attacker.invalid/cb?code=C"))),
            AuthErrorKind::malformed);
}

TEST_F(RedirectCredential, IsRefusedWhenTheRedirectCarriesTwoCodes) {
  EXPECT_EQ(AuthRig::kind_of(rig.code(AuthRig::redirect(
                "https://psnow.playstation.com/app/2.2.0/133/5cdcc037d/"
                "grc-response.html?code=first&code=second"))),
            AuthErrorKind::malformed);
}

}  // namespace
}  // namespace psn_test
