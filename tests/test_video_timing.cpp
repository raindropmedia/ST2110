#include "core/SyntheticSender.h"
#include "core/VideoPacketAnalyzer.h"
#include "core/VideoTimingModel.h"

#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

namespace {

// Epoche weit von 0 entfernt, wie bei TAI-Zeit (≈ 1,7e18 ns).
constexpr uint64_t kBaseFrame = 88'000'000'000ULL;

std::vector<FrameTiming> run(const VideoFormat& format, const SenderFaults& faults, int frames,
                             VideoPacketAnalyzer& analyzer, Path path = Path::Primary) {
    SyntheticSender sender(format, AudioFormat{}, 7);
    std::vector<FrameTiming> out;
    for (int f = 0; f < frames; ++f) {
        sender.videoFrame(kBaseFrame + f, faults, 0.0, [&](uint32_t rtp, uint64_t ns, int idx) {
            if (auto done = analyzer.onPacket(path, rtp, ns, idx)) {
                out.push_back(*done);
            }
        });
    }
    if (auto done = analyzer.flush(path)) {
        out.push_back(*done);
    }
    return out;
}

} // namespace

TEST_CASE("Kenngrößen 1080p50 wie MTL") {
    auto p = computeVideoTimingParams(1080, false, FrameRate::P50, 4320);
    CHECK(p.frameTimeNs == doctest::Approx(20'000'000.0));
    CHECK(p.trsNs == doctest::Approx(20e6 * 1080.0 / 1125.0 / 4320.0));
    CHECK(p.trOffsetNs == doctest::Approx(20e6 * 43.0 / 1125.0));
    CHECK(p.pass.cinstMaxNarrow == 5);
    CHECK(p.pass.cinstMaxWide == 16);
    CHECK(p.pass.vrxMaxNarrow == 8);
    CHECK(p.pass.vrxMaxWide == 720);
    CHECK(p.pass.rtpTsDeltaMin == 1800);
    CHECK(p.pass.rtpTsDeltaMax == 1801);
    CHECK(p.pass.rtpOffsetMax == 70);
}

TEST_CASE("Kenngrößen 1080i59.94 und 2160p59.94") {
    auto i = computeVideoTimingParams(1080, true, FrameRate::P59_94, 2160);
    CHECK(i.trOffsetNs == doctest::Approx(1e9 * 1001 / 60000.0 * 22.0 / 1125.0 * 2));
    CHECK(i.pass.rtpTsDeltaMin == 1501);
    auto uhd = computeVideoTimingParams(2160, false, FrameRate::P59_94, 17280);
    CHECK(uhd.trsNs == doctest::Approx(1e9 * 1001 / 60000.0 * 1080.0 / 1125.0 / 17280.0));
    CHECK(uhd.pass.cinstMaxNarrow > 5);
}

TEST_CASE("Paketzahl je Bild") {
    CHECK(packetsPerFrame(VideoFormat{1920, 1080, FrameRate::P50, false, PixelFormat{}}) == 4320);
    CHECK(packetsPerFrame(VideoFormat{1920, 1080, FrameRate::P50, true, PixelFormat{}}) == 2160);
    CHECK(packetsPerFrame(VideoFormat{3840, 2160, FrameRate::P50, false, PixelFormat{}}) == 17280);
}

TEST_CASE("Urteil folgt der Reihenfolge von MTL") {
    auto p = computeVideoTimingParams(1080, false, FrameRate::P50, 4320).pass;
    FrameTiming f;
    f.fptNs = 1000;
    f.latencyNs = 1000;
    f.rtpTsDelta = 1800;
    f.vrxMax = 2;
    f.cinstMax = 2;
    std::string cause;
    CHECK(evaluateVideoFrame(f, p, cause) == Compliance::Narrow);
    f.cinstMax = 10;
    CHECK(evaluateVideoFrame(f, p, cause) == Compliance::Wide);
    CHECK(cause == "wide as cinst exceed narrow max");
    f.cinstMax = 2;
    f.vrxMax = 20;
    CHECK(evaluateVideoFrame(f, p, cause) == Compliance::Wide);
    f.vrxMin = -1;
    CHECK(evaluateVideoFrame(f, p, cause) == Compliance::Failed);
    CHECK(cause == "vrx exceed min");
    f.vrxMin = 0;
    f.fptNs = p.trOffsetNs + 1;
    CHECK(evaluateVideoFrame(f, p, cause) == Compliance::Failed);
    CHECK(cause == "fpt exceed tr_offset");
}

TEST_CASE("Idealer Sender ist narrow, Paketzahl wird gelernt") {
    const VideoFormat format{1920, 1080, FrameRate::P50, false, PixelFormat{}};
    VideoPacketAnalyzer analyzer(format.height, format.interlaced, format.rate);
    auto frames = run(format, SenderFaults{}, 6, analyzer);
    REQUIRE(analyzer.params());
    CHECK(analyzer.params()->packetsPerFrame == 4320);
    REQUIRE(frames.size() >= 3);
    for (const auto& f : frames) {
        INFO(f.failedCause, " fpt=", f.fptNs, " lat=", f.latencyNs, " vrx=", f.vrxMin, "..", f.vrxMax,
             " cinst=", f.cinstMax, " d=", f.rtpTsDelta);
        CHECK(f.compliance == Compliance::Narrow);
        CHECK(f.packets == 4320);
        CHECK(f.cinstMax <= 1);
        CHECK(f.vrxMin >= 0);
        CHECK(f.rtpOffset == 0);
        CHECK(f.iptAvgNs == doctest::Approx(analyzer.params()->trsNs).epsilon(0.01));
    }
    const auto& h = analyzer.totals(Path::Primary);
    CHECK(h.iptNs.count() > 0);
    // IPT ganzzahlig: höchstens 1 ns Abweichung von TRS (keine 256-ns-Stufen).
    CHECK(h.iptNs.max() - h.iptNs.min() <= 2.0);
    CHECK(analyzer.lastProfile(Path::Primary).index.size() == 4320);
}

TEST_CASE("Bündel ergeben wide, späte Pakete failed") {
    const VideoFormat format{1920, 1080, FrameRate::P50, false, PixelFormat{}};
    SUBCASE("Bündel zu 8 Paketen") {
        VideoPacketAnalyzer analyzer(format.height, format.interlaced, format.rate);
        analyzer.setPacketsPerFrame(4320);
        SenderFaults faults;
        faults.burstPackets = 8;
        auto frames = run(format, faults, 3, analyzer);
        REQUIRE_FALSE(frames.empty());
        CHECK(frames.back().compliance == Compliance::Wide);
        CHECK(frames.back().cinstMax >= 6);
        CHECK(analyzer.worstProfile(Path::Primary).timing.cinstMax >= 6);
    }
    SUBCASE("Start 100 µs zu spät") {
        VideoPacketAnalyzer analyzer(format.height, format.interlaced, format.rate);
        analyzer.setPacketsPerFrame(4320);
        SenderFaults faults;
        faults.startOffsetNs = 100000;
        auto frames = run(format, faults, 3, analyzer);
        REQUIRE_FALSE(frames.empty());
        CHECK(frames.back().compliance == Compliance::Failed);
        CHECK(frames.back().failedCause == "fpt exceed tr_offset");
    }
}

TEST_CASE("Intervall-Histogramme werden geleert, Gesamtwerte bleiben") {
    const VideoFormat format{1280, 720, FrameRate::P50, false, PixelFormat{}};
    VideoPacketAnalyzer analyzer(format.height, format.interlaced, format.rate);
    analyzer.setPacketsPerFrame(packetsPerFrame(format));
    run(format, SenderFaults{}, 2, analyzer);
    auto interval = analyzer.takeInterval(Path::Primary);
    CHECK(interval.iptNs.count() > 0);
    CHECK(analyzer.takeInterval(Path::Primary).iptNs.count() == 0);
    CHECK(analyzer.totals(Path::Primary).iptNs.count() == interval.iptNs.count());
    analyzer.resetTotals();
    CHECK(analyzer.totals(Path::Primary).iptNs.count() == 0);
}
