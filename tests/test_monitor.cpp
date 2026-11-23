#include "core/MonitorHub.h"
#include "core/SyntheticSender.h"

#include <doctest/doctest.h>

using namespace st2110;

namespace {

struct FakeClock {
    double t = 0;
};

FlowConfig videoFlow(bool redundant) {
    FlowConfig f;
    std::string error;
    parseFlowSpec(Media::Video,
                  redundant ? "239.0.0.1:20000,red=239.0.1.1:20000,name=Kamera 1"
                            : "239.0.0.1:20000,name=Kamera 1",
                  f, error);
    return f;
}

FlowConfig audioFlow() {
    FlowConfig f;
    std::string error;
    parseFlowSpec(Media::Audio, "239.0.0.2:30000,name=Ton 1", f, error);
    return f;
}

HubConfig demoHub() {
    HubConfig c;
    c.demo = true;
    c.maxFlows = 3;
    return c;
}

} // namespace

TEST_CASE("Hub: Flows anlegen, IDs vergeben, Doppelte und Höchstzahl ablehnen") {
    FakeClock clock;
    MonitorHub hub(demoHub(), [&] { return clock.t; });
    std::string error;
    auto v = hub.addFlow(videoFlow(true), error);
    REQUIRE(v);
    CHECK(v->id() == "v1");
    auto a = hub.addFlow(audioFlow(), error);
    REQUIRE(a);
    CHECK(a->id() == "a1");
    CHECK_FALSE(hub.addFlow(videoFlow(false), error));
    CHECK(error.find("bereits") != std::string::npos);
    FlowConfig other;
    parseFlowSpec(Media::Video, "239.0.0.9:20000", other, error);
    auto third = hub.addFlow(other, error);
    REQUIRE(third);
    CHECK(third->config().label == "Video 239.0.0.9");
    FlowConfig fourth;
    parseFlowSpec(Media::Audio, "239.0.0.10:20000", fourth, error);
    CHECK_FALSE(hub.addFlow(fourth, error));
    CHECK(error.find("Höchstzahl") != std::string::npos);
    CHECK(hub.removeFlow("v2"));
    CHECK_FALSE(hub.removeFlow("v2"));
    CHECK(hub.flows().size() == 2);
    auto flows = hub.flowsJson();
    CHECK(flows["monitored"].size() == 2);
    CHECK(flows["monitored"][0]["id"] == "v1");
    CHECK(flows["monitored"][0]["flows"]["R"]["ip"] == "239.0.1.1");
}

TEST_CASE("Flow: Demo-Daten ergeben Urteile, Histogramme, Versatz, Raten und Ereignisse") {
    FakeClock clock;
    MonitorHub hub(demoHub(), [&] { return clock.t; });
    std::string error;
    auto video = hub.addFlow(videoFlow(true), error);
    auto audio = hub.addFlow(audioFlow(), error);
    REQUIRE(video);
    REQUIRE(audio);
    SyntheticSender sender(VideoFormat{}, AudioFormat{}, 5);
    const uint64_t base = 88'000'000'000ULL;
    StreamCounters counters;
    for (int f = 0; f < 120; ++f) {
        clock.t = f * 0.02;
        for (Path path : {Path::Primary, Path::Redundant}) {
            const double delay = path == Path::Redundant ? 12000.0 : 0.0;
            sender.videoFrame(base + f, SenderFaults{}, delay, [&](uint32_t rtp, uint64_t ns, int idx) {
                video->onVideoPacket(path, rtp, ns, idx, true);
            });
            counters.paths[pathIndex(path)].packets += 4320;
            counters.paths[pathIndex(path)].bytes += 4320 * 1262;
        }
        for (int k = 0; k < 20; ++k) {
            sender.audioPacket(base * 20 + f * 20 + k, SenderFaults{}, 0.0, [&](uint32_t rtp, uint64_t ns) {
                audio->onAudioPacket(Path::Primary, rtp, ns, true);
            });
        }
        if (f % 16 == 0) {
            hub.onPtpSync(20, 37);
        }
        if (f % 25 == 0) {
            video->setCounters(counters);
        }
    }
    auto snap = hub.snapshot("v1", 60.0);
    CHECK(snap["demo"] == true);
    REQUIRE(snap["flows"].size() == 2);
    const auto& summary = snap["flows"][0];
    CHECK(summary["verdict"]["P"] == "narrow");
    CHECK(summary["verdict"]["R"] == "narrow");
    CHECK(summary["rates"]["P"]["pps"].get<double>() == doctest::Approx(216000).epsilon(0.01));
    CHECK(summary["rates"]["P"]["bps"].get<double>() > 2e9);
    const auto& detail = snap["flow"];
    CHECK(detail["id"] == "v1");
    CHECK(detail["kind"] == "video");
    const auto& v = detail["video"];
    CHECK(v["frameSource"] == "analyzer");
    CHECK(v["params"]["packetsPerFrame"] == 4320);
    CHECK(v["paths"]["P"]["histograms"]["iptNs"]["count"].get<uint64_t>() > 100000);
    CHECK(v["paths"]["P"]["profiles"]["last"]["index"].size() <= 2000);
    CHECK(v["skew"]["lastNs"].get<int64_t>() == doctest::Approx(12000).epsilon(0.01));
    CHECK(snap["ptp"]["state"] == "locked");
    CHECK_FALSE(snap["events"].empty());
    bool flowEvent = false;
    for (const auto& e : snap["events"]) {
        flowEvent =
            flowEvent || (e["flow"] == "v1" && e["text"].get<std::string>().rfind("Kamera 1: ", 0) == 0);
    }
    CHECK(flowEvent);

    auto audioSnap = hub.snapshot("a1", 60.0);
    CHECK(audioSnap["flow"]["kind"] == "audio");
    CHECK(audioSnap["flow"]["audio"]["paths"]["P"]["verdict"] == "narrow");

    auto interval = hub.takeIntervalRecord();
    CHECK(interval["type"] == "interval");
    CHECK(interval["flows"]["v1"]["paths"]["P"]["verdict"] == "narrow");
    CHECK_FALSE(interval["flows"]["v1"]["paths"]["P"]["histograms"]["iptNs"].is_null());
    CHECK_FALSE(interval["flows"]["v1"]["skewNs"].is_null());
    CHECK(interval["flows"]["a1"]["kind"] == "audio");
    auto next = hub.takeIntervalRecord();
    CHECK(next["flows"]["v1"]["paths"]["P"]["frames"] == 0);
    auto summaryRecord = hub.summaryRecord();
    CHECK(summaryRecord["flows"]["v1"]["paths"]["P"]["frames"].get<uint64_t>() > 100);
    CHECK(hub.received() > 100);
    CHECK(hub.startRecord()["flows"]["v1"]["format"] == "1920x1080p50");
}

TEST_CASE("Flow: MTL-Bilder haben Vorrang vor der eigenen Analyse") {
    MonitorHub hub(HubConfig{});
    std::string error;
    auto video = hub.addFlow(videoFlow(true), error);
    REQUIRE(video);
    VideoFrameReport report;
    report.status = FrameStatus::Corrupted;
    report.packetsTotal = 4000;
    FrameTiming t;
    t.compliance = Compliance::Wide;
    report.timing[0] = t;
    video->onVideoFrame(report);
    auto snap = hub.snapshot("v1");
    CHECK(snap["flow"]["video"]["frameSource"] == "mtl");
    CHECK(snap["flow"]["video"]["frames"]["corrupted"] == 1);
    bool found = false;
    for (const auto& e : snap["events"]) {
        found = found || e["text"].get<std::string>().find("beschädigt") != std::string::npos;
    }
    CHECK(found);
}

TEST_CASE("Flow: Latenz an der int32-Grenze meldet abweichende Zeitbasen") {
    MonitorHub hub(HubConfig{});
    std::string error;
    auto video = hub.addFlow(videoFlow(true), error);
    REQUIRE(video);
    auto frame = [&](int32_t latencyNs, int32_t rtpOffset) {
        VideoFrameReport report;
        report.packetsTotal = 4115;
        FrameTiming t;
        t.compliance = Compliance::Failed;
        t.latencyNs = latencyNs;
        t.rtpOffset = rtpOffset;
        report.timing[0] = t;
        video->onVideoFrame(report);
    };
    auto count = [&](const std::string& part) {
        int n = 0;
        const auto snap = hub.snapshot("v1"); // nicht als Temporäres durchlaufen
        for (const auto& e : snap["events"]) {
            n += e["text"].get<std::string>().find(part) != std::string::npos ? 1 : 0;
        }
        return n;
    };
    frame(7200, 67); // gesund
    CHECK(count("weichen um mehr als 2 s ab") == 0);
    frame(INT32_MIN, -385133); // gemessen auf der Karte
    frame(INT32_MIN, -385133); // nur der Wechsel wird gemeldet
    CHECK(count("weichen um mehr als 2 s ab") == 1);
    CHECK(count("-385133") == 1);
    frame(7200, 67);
    CHECK(count("stimmen wieder überein") == 1);
}

TEST_CASE("Flow: Zähler melden Verluste, Zurücksetzen leert") {
    MonitorHub hub(demoHub());
    std::string error;
    auto video = hub.addFlow(videoFlow(true), error);
    StreamCounters c;
    c.paths[0].packets = 100;
    video->setCounters(c);
    c.paths[0].lost = 3;
    c.unrecovered = 1;
    video->setCounters(c);
    auto snap = hub.snapshot("v1");
    CHECK(snap["flow"]["counters"]["paths"]["P"]["lost"] == 3);
    CHECK(snap["flows"][0]["lost"] == 3);
    auto interval = hub.takeIntervalRecord();
    CHECK(interval["flows"]["v1"]["counters"]["paths"]["P"]["lost"] == 3);
    hub.reset();
    CHECK(hub.snapshot("v1")["events"].size() == 1);
    // MTL zählt weiter: nach dem Zurücksetzen zählt nur der Zuwachs.
    snap = hub.snapshot("v1");
    CHECK(snap["flow"]["counters"]["paths"]["P"]["lost"] == 0);
    CHECK(snap["flow"]["counters"]["unrecovered"] == 0);
    CHECK(snap["flows"][0]["lost"] == 0);
    c.paths[0].packets = 150;
    c.paths[0].lost = 5;
    video->setCounters(c);
    snap = hub.snapshot("v1");
    CHECK(snap["flow"]["counters"]["paths"]["P"]["lost"] == 2);
    CHECK(snap["flow"]["counters"]["paths"]["P"]["packets"] == 50);
    CHECK(snap["flow"]["counters"]["unrecovered"] == 0);
    CHECK(snap["flows"][0]["lost"] == 2);
}

TEST_CASE("Hub: Snapshot ohne Auswahl zeigt den ersten Flow, unbekannte ID keinen") {
    MonitorHub hub(demoHub());
    std::string error;
    hub.addFlow(audioFlow(), error);
    CHECK(hub.snapshot("")["flow"]["id"] == "a1");
    CHECK(hub.snapshot("x9")["flow"].is_null());
}
