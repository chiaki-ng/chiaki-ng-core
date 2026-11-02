#include "tests/unit/core/catalog/details_rig.hpp"

#include <libchiaking/catalog_types.hpp>

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace catalog_test {
namespace {

using libchiaking::catalog::ImageRef;
using libchiaking::catalog::TrailerRef;

TEST(GameImageRef, AdmitsPsImageHosts) {
  EXPECT_TRUE(ImageRef::at(DetailsRig::concept_logo));
  EXPECT_TRUE(ImageRef::at(DetailsRig::rating_icon));
  EXPECT_TRUE(ImageRef::at(std::string{DetailsRig::concept_logo} + "?w=440"));
  for (const std::string_view admitted :
       {"https://vulcan.dl.playstation.net/ap/rnd/202303/2109/"
        "8f16f54304b3321ceb401aaaf6c1fcf3.png",
        "https://apollo2.dl.playstation.net/cdn/EP0100/CUSA07011_00/"
        "a3pL34y0wJ2xou9myR8f0gTHnLk63ndy.png",
        "https://gs2-sec.ww.prod.dl.playstation.net/gs2-sec/appkgo/prod/"
        "CUSA07040_00/1/i_691ab247.png",
        "https://psnobj.prod.dl.playstation.net/psnobj/NPWR00000_00/"
        "00000000-0000-0000-0000-000000000000.png",
        "https://image.api.np.km.playstation.net/images/"
        "?format=png&image=http%3A%2F%2Fgs2-sec.ww.prod."
        "dl.playstation.net%2Fgs2-sec%2Fappkgo%2Fprod%2FCUSA09239_00%2F1%2Fi_"
        "691ab247.png&w=440"})
    EXPECT_TRUE(ImageRef::at(admitted)) << admitted;
}

TEST(GameImageRef, RefusesAnythingElse) {
  for (const std::string_view refused :
       {"", "https://image.api.playstation.com/",
        "http://image.api.playstation.com/a.png",
        "https://image.api.playstation.com.evil.example/a.png",
        "https://evil.example/image.api.playstation.com/a.png",
        "https://user@image.api.playstation.com/a.png",
        "https://image.api.playstation.com@evil.example/a.png",
        "https://image.api.playstation.com:8443/a.png",
        "https://image.api.playstation.com/a/../b.png",
        "https://image.api.playstation.com/a b.png",
        "https://image.api.playstation.com/a.png#x",
        "https://apollo2.dl.playstation.net.evil.example/a.png",
        "http://vulcan.dl.playstation.net/a.png",
        "https://image.api.np.km.playstation.net/../a.png",
        "https://store.playstation.com/en-us/product/"
        "EP0109-PPSA17001_00-FAKE0056TITLE000"})
    EXPECT_FALSE(ImageRef::at(refused)) << refused;
}

TEST(GameImageRef, RefusesAnOverlongAddress) {
  EXPECT_FALSE(ImageRef::at("https://image.api.playstation.com/" +
                            std::string(600, 'a')));
}

TEST(GameTrailerRef, AdmitsOnlyPsVideos) {
  EXPECT_TRUE(TrailerRef::at(DetailsRig::product_trailer));
  EXPECT_FALSE(TrailerRef::at(DetailsRig::concept_logo));
  EXPECT_FALSE(TrailerRef::at("https://evil.example/a.mp4"));
}

}  // namespace
}  // namespace catalog_test
