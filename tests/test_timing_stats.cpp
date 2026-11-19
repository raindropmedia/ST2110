#include "core/TimingStats.h"

#include <doctest/doctest.h>
#include <string>

using namespace st2110;

namespace {

FrameTiming frame(Compliance compliance, int32_t cinstMax, int32_t fpt, const std::string& cause = {}) {
    FrameTiming f;
    f.cinstMin = 0;
    f.cinstMax = cinstMax;
    f.cinstAvg = static_cast<float>(cinstMax) / 2.0f;
    f.vrxMin = 2;
    f.vrxMax = 8;
    f.vrxAvg = 5;
    f.iptMinNs = 4000;
    f.iptMaxNs = 5000;
    f.iptAvgNs = 4400;
    f.fptNs = fpt;
    f.latencyNs = 30000;
    f.rtpOffset = 10;
    f.rtpTsDelta = 1800;
    f.compliance = compliance;
    f.failedCause = cause;
    f.packets = 4320;
    return f;
}

} // namespace

TEST_CASE("Leere Statistik ist failed und ohne Werte") {
    TimingStats stats;
    CHECK(stats.frames() == 0);
    CHECK(stats.worst() == Compliance::Failed);
    CHECK(stats.cinst().empty());
}

TEST_CASE("Min, Max und Mittel über Bilder") {
    TimingStats stats;
    stats.add(frame(Compliance::Narrow, 4, 10000));
    stats.add(frame(Compliance::Narrow, 8, 20000));
    CHECK(stats.frames() == 2);
    CHECK(stats.packets() == 8640);
    CHECK(stats.worst() == Compliance::Narrow);
    CHECK(stats.cinst().min() == 0);
    CHECK(stats.cinst().max() == 8);
    CHECK(stats.cinst().mean() == doctest::Approx(3.0));
    CHECK(stats.fptNs().min() == 10000);
    CHECK(stats.fptNs().max() == 20000);
    CHECK(stats.fptNs().mean() == doctest::Approx(15000.0));
    CHECK(stats.failedCauses().empty());
}

TEST_CASE("Schlechtestes Urteil gewinnt") {
    TimingStats stats;
    stats.add(frame(Compliance::Narrow, 4, 1));
    stats.add(frame(Compliance::Wide, 4, 1, "cinst exceeds narrow"));
    CHECK(stats.worst() == Compliance::Wide);
    stats.add(frame(Compliance::Failed, 40, 1, "vrx overflow"));
    CHECK(stats.worst() == Compliance::Failed);
    CHECK(stats.count(Compliance::Narrow) == 1);
    CHECK(stats.count(Compliance::Wide) == 1);
    CHECK(stats.count(Compliance::Failed) == 1);
    CHECK(stats.failedCauses().at("vrx overflow") == 1);
}

TEST_CASE("Fehlerursachen sind begrenzt") {
    TimingStats stats;
    for (size_t i = 0; i < TimingStats::kMaxCauses + 5; ++i) {
        stats.add(frame(Compliance::Failed, 1, 1, "Ursache " + std::to_string(i)));
    }
    CHECK(stats.failedCauses().size() == TimingStats::kMaxCauses + 1);
    CHECK(stats.failedCauses().at(TimingStats::kOtherCauses) == 5);
}

TEST_CASE("Zusammenführen ergibt dieselben Werte wie direkte Erfassung") {
    TimingStats a;
    TimingStats b;
    TimingStats direct;
    for (int i = 0; i < 3; ++i) {
        a.add(frame(Compliance::Narrow, 4 + i, 1000 * i));
        direct.add(frame(Compliance::Narrow, 4 + i, 1000 * i));
    }
    for (int i = 0; i < 2; ++i) {
        b.add(frame(Compliance::Wide, 20 + i, 5000, "wide"));
        direct.add(frame(Compliance::Wide, 20 + i, 5000, "wide"));
    }
    TimingStats merged;
    merged.merge(a);
    merged.merge(TimingStats{});
    merged.merge(b);
    CHECK(merged.frames() == direct.frames());
    CHECK(merged.packets() == direct.packets());
    CHECK(merged.worst() == direct.worst());
    CHECK(merged.cinst().max() == direct.cinst().max());
    CHECK(merged.cinst().mean() == doctest::Approx(direct.cinst().mean()));
    CHECK(merged.fptNs().min() == direct.fptNs().min());
    CHECK(merged.failedCauses() == direct.failedCauses());
}

TEST_CASE("Bildzähler") {
    FrameCounters a{10, 1, 2, 0};
    FrameCounters b{5, 0, 0, 3};
    a.merge(b);
    CHECK(a.total() == 21);
    CHECK(a.dropped == 3);
}
