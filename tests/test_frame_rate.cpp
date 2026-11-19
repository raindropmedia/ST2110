#include "core/FrameRate.h"

#include <doctest/doctest.h>

using namespace st2110;

TEST_CASE("Bildraten werden über ihren Namen gelesen") {
    CHECK(parseFrameRate("50") == FrameRate::P50);
    CHECK(parseFrameRate("59.94") == FrameRate::P59_94);
    CHECK(parseFrameRate("23.98") == FrameRate::P23_98);
    CHECK_FALSE(parseFrameRate("59,94").has_value());
    CHECK_FALSE(parseFrameRate("").has_value());
    CHECK(frameRateName(FrameRate::P119_88) == "119.88");
}

TEST_CASE("Bilddauer folgt der Rate") {
    CHECK(framePeriodNs(FrameRate::P50) == doctest::Approx(20'000'000.0));
    CHECK(framePeriodNs(FrameRate::P59_94) == doctest::Approx(16'683'333.3).epsilon(1e-6));
}

TEST_CASE("Alle Namen lassen sich zurücklesen") {
    for (auto rate : {FrameRate::P23_98, FrameRate::P24, FrameRate::P25, FrameRate::P29_97, FrameRate::P30,
                      FrameRate::P50, FrameRate::P59_94, FrameRate::P60, FrameRate::P100, FrameRate::P119_88,
                      FrameRate::P120}) {
        CHECK(parseFrameRate(frameRateName(rate)) == rate);
    }
}
