#include "core/Monitor.h"
#include "core/SyntheticSender.h"

#include <doctest/doctest.h>

using namespace st2110;

namespace {

struct FakeClock {
    double t = 0;
};

MonitorConfig demoConfig() {
    MonitorConfig c;
    VideoFlow v;
    v.primary = FlowAddress{"239.0.0.1", 20000, {}};
    v.redundant = FlowAddress{"239.0.1.1", 20000, {}};
    c.video = v;
    AudioFlow a;
    a.primary = FlowAddress{"239.0.0.2", 30000, {}};
    c.audio = a;
    c.demo = true;
    return c;
}

} // namespace

TEST_CASE("Monitor: Demo-Daten ergeben Urteile, Histogramme, Versatz und Ereignisse") {
    FakeClock clock;
    Monitor monitor(demoConfig(), [&] { return clock.t; });
    SyntheticSender sender(VideoFormat{}, AudioFormat{}, 5);
    const uint64_t base = 88'000'000'000ULL;
    for (int f = 0; f < 120; ++f) {
        clock.t = f * 0.02;
        for (Path path : {Path::Primary, Path::Redundant}) {
            const double delay = path == Path::Redundant ? 12000.0 : 0.0;
            sender.videoFrame(base + f, SenderFaults{}, delay, [&](uint32_t rtp, uint64_t ns, int idx) {
                monitor.onVideoPacket(path, rtp, ns, idx, true);
            });
        }
        for (int k = 0; k < 20; ++k) {
            sender.audioPacket(base * 20 + f * 20 + k, SenderFaults{}, 0.0, [&](uint32_t rtp, uint64_t ns) {
                monitor.onAudioPacket(Path::Primary, rtp, ns, true);
            });
        }
        if (f % 16 == 0) {
            monitor.onPtpSync(20, 37);
        }
    }
    auto snap = monitor.snapshot(60.0);
    CHECK(snap["demo"] == true);
    const auto& video = snap["video"];
    CHECK(video["frameSource"] == "analyzer");
    CHECK(video["params"]["packetsPerFrame"] == 4320);
    CHECK(video["paths"]["P"]["verdict"] == "narrow");
    CHECK(video["paths"]["R"]["verdict"] == "narrow");
    CHECK(video["paths"]["P"]["histograms"]["iptNs"]["count"].get<uint64_t>() > 100000);
    CHECK(video["paths"]["P"]["profiles"]["last"]["packets"] == 4320);
    CHECK(video["paths"]["P"]["profiles"]["last"]["index"].size() <= 2000);
    CHECK(video["skew"]["lastNs"].get<int64_t>() == doctest::Approx(12000).epsilon(0.01));
    CHECK(snap["audio"]["paths"]["P"]["verdict"] == "narrow");
    CHECK(snap["ptp"]["state"] == "locked");
    CHECK_FALSE(snap["events"].empty());

    auto interval = monitor.takeIntervalRecord();
    CHECK(interval["type"] == "interval");
    CHECK(interval["video"]["paths"]["P"]["verdict"] == "narrow");
    CHECK_FALSE(interval["video"]["paths"]["P"]["histograms"]["iptNs"].is_null());
    CHECK_FALSE(interval["video"]["skewNs"].is_null());
    auto next = monitor.takeIntervalRecord();
    CHECK(next["video"]["paths"]["P"]["frames"] == 0);
    auto summary = monitor.summaryRecord();
    CHECK(summary["video"]["paths"]["P"]["frames"].get<uint64_t>() > 100);
    CHECK(monitor.videoFrames() > 100);
}

TEST_CASE("Monitor: MTL-Bilder haben Vorrang vor der eigenen Analyse") {
    FakeClock clock;
    MonitorConfig config = demoConfig();
    config.demo = false;
    Monitor monitor(config, [&] { return clock.t; });
    VideoFrameReport report;
    report.status = FrameStatus::Corrupted;
    report.packetsTotal = 4000;
    FrameTiming t;
    t.compliance = Compliance::Wide;
    t.failedCause = "wide as cinst exceed narrow max";
    report.timing[0] = t;
    monitor.onVideoFrame(report);
    auto snap = monitor.snapshot();
    CHECK(snap["video"]["frameSource"] == "mtl");
    CHECK(snap["video"]["frames"]["corrupted"] == 1);
    bool found = false;
    for (const auto& e : snap["events"]) {
        found = found || e["text"].get<std::string>().find("beschädigt") != std::string::npos;
    }
    CHECK(found);
}

TEST_CASE("Monitor: Zähler melden Verluste, Zurücksetzen leert") {
    Monitor monitor(demoConfig());
    StreamCounters c;
    c.paths[0].packets = 100;
    monitor.setCounters(Media::Video, c);
    c.paths[0].lost = 3;
    c.unrecovered = 1;
    monitor.setCounters(Media::Video, c);
    auto snap = monitor.snapshot();
    CHECK(snap["counters"]["video"]["paths"]["P"]["lost"] == 3);
    CHECK(snap["events"].size() >= 2);
    auto interval = monitor.takeIntervalRecord();
    CHECK(interval["counters"]["video"]["paths"]["P"]["lost"] == 3);
    monitor.reset();
    auto after = monitor.snapshot();
    CHECK(after["events"].size() == 1);
}
