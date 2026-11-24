#include "core/AncAnalyzer.h"
#include "core/FlowMonitor.h"
#include "core/HbrmtAnalyzer.h"
#include "core/MonitorHub.h"
#include "core/Options.h"

#include <cstring>
#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

namespace {

AncFrameReport ancFrame(uint32_t rtp, uint64_t arrivalNs, std::vector<AncPacket> packets) {
    AncFrameReport f;
    f.rtpTimestamp = rtp;
    f.firstPacketNs = arrivalNs;
    f.packets = 1;
    f.marker = true;
    f.anc = std::move(packets);
    return f;
}

// RTP-Paket nach ST 2022-6: 12 Byte RTP, 8 Byte Header (CF=1: +4 Byte Video-Zeitstempel), 1376 Byte Nutzlast.
std::vector<uint8_t> hbrmtPacket(uint16_t seq, uint32_t ts, bool marker, uint8_t frCount,
                                 uint8_t frame = 0x21, uint8_t frate = 0x12, uint8_t sample = 0x01,
                                 uint8_t cf = 1, size_t payload = 1376) {
    std::vector<uint8_t> p(12 + 8 + (cf ? 4 : 0) + payload, 0);
    p[0] = 0x80;
    p[1] = static_cast<uint8_t>((marker ? 0x80 : 0) | 98);
    p[2] = static_cast<uint8_t>(seq >> 8);
    p[3] = static_cast<uint8_t>(seq);
    for (int i = 0; i < 4; ++i) {
        p[4 + i] = static_cast<uint8_t>(ts >> (24 - 8 * i));
    }
    p[8] = 0x12;
    p[9] = 0x34;
    p[10] = 0x56;
    p[11] = 0x78;
    // Wort 0: Ext=0, F=1, VSID=0, FRCount, R=2 (UTC), S=0, FEC=0, CF, RESERVE=0
    const uint32_t w0 =
        (1u << 27) | (static_cast<uint32_t>(frCount) << 16) | (2u << 14) | (static_cast<uint32_t>(cf) << 5);
    const uint32_t w1 = (0u << 28) | (static_cast<uint32_t>(frame) << 20) |
                        (static_cast<uint32_t>(frate) << 12) | (static_cast<uint32_t>(sample) << 8);
    for (int i = 0; i < 4; ++i) {
        p[12 + i] = static_cast<uint8_t>(w0 >> (24 - 8 * i));
        p[16 + i] = static_cast<uint8_t>(w1 >> (24 - 8 * i));
    }
    if (cf) {
        for (int i = 0; i < 4; ++i) {
            p[20 + i] = static_cast<uint8_t>(ts >> (24 - 8 * i)); // Video-Zeitstempel = RTP-Zeitstempel
        }
    }
    return p;
}

} // namespace

TEST_CASE("ST 2110-40: Name der Datenarten") {
    CHECK(std::string(ancDataName(0x41, 0x01)) == "Payload Identification (ST 352)");
    CHECK(std::string(ancDataName(0x60, 0x60)) == "Zeitcode ATC (ST 12-2)");
    CHECK(std::string(ancDataName(0x61, 0x01)) == "Untertitel CEA-708 (CDP)");
    CHECK(std::string(ancDataName(0x99, 0x99)).empty());
}

TEST_CASE("ST 2110-40: Zählung, Zeitbezug und Zeitstempelschritt") {
    VideoFormat format;
    format.rate = FrameRate::P50;
    AncAnalyzer a(format);
    const uint64_t base = 1'700'000'000ULL * 1'000'000'000ULL;
    // 50 Hz: 1800 Takte je Bild, erste Pakete 300 µs nach der Sollzeit (ganze Bildperioden: 20 ms)
    uint32_t rtp = 90000u * 1000u;
    for (int i = 0; i < 10; ++i) {
        const uint64_t arrival = base + 20'000'000ULL * i + 300'000ULL;
        // rtp passend zur PTP-Zeit: (base + 20 ms * i) in 90-kHz-Takten
        const uint64_t seconds = (base + 20'000'000ULL * i) / 1'000'000'000ULL;
        const uint64_t rest = (base + 20'000'000ULL * i) % 1'000'000'000ULL;
        rtp = static_cast<uint32_t>(seconds * 90000ULL + rest * 90000ULL / 1'000'000'000ULL);
        AncPacket p;
        p.did = 0x61;
        p.sdid = 0x01;
        p.line = 9;
        p.udwSize = 40;
        AncPacket q;
        q.did = 0x41;
        q.sdid = 0x05;
        q.line = 11;
        q.udwSize = 8;
        const auto r = a.add(ancFrame(rtp, arrival, {p, q}), 1.0 + i * 0.02);
        CHECK(r.level == AncAnalyzer::Level::Ok);
    }
    const auto j = a.toJson(2.0, 60, 100);
    CHECK(j["frames"]["total"] == 10);
    CHECK(j["ancPackets"] == 20);
    CHECK(j["verdict"] == "ok");
    CHECK(j["data"].size() == 2);
    CHECK(j["data"][0]["count"] == 10);
    CHECK(j["rtpStep"]["min"] == 1800);
    CHECK(j["rtpStep"]["max"] == 1800);
    CHECK(j["latency"]["minUs"].get<double>() == doctest::Approx(300).epsilon(0.05));
    CHECK(j["perFrame"]["max"] == 2);
    CHECK(j["system10"].size() == 2);
}

TEST_CASE("ST 2110-40: Verluste, fehlender Marker und falscher Schritt") {
    VideoFormat format;
    format.rate = FrameRate::P50;
    AncAnalyzer a(format);
    AncFrameReport f = ancFrame(1000, 0, {});
    CHECK(a.add(f, 1.0).level == AncAnalyzer::Level::Ok);
    f.rtpTimestamp = 1000 + 1800;
    f.seqLost = 2;
    f.seqDiscont = true;
    auto r = a.add(f, 1.02);
    CHECK(r.level == AncAnalyzer::Level::Fail);
    CHECK(a.verdict(1.03) == AncAnalyzer::Level::Fail);
    CHECK(a.verdict(20.0) == AncAnalyzer::Level::Ok); // nach 10 s ohne Fehler wieder in Ordnung
    f.seqLost = 0;
    f.seqDiscont = false;
    f.marker = false;
    f.rtpTimestamp += 1800;
    CHECK(a.add(f, 21.0).level == AncAnalyzer::Level::Warn);
    f.marker = true;
    f.rtpTimestamp += 900; // halber Schritt
    r = a.add(f, 21.02);
    CHECK(r.level == AncAnalyzer::Level::Warn);
    CHECK(a.toJson(21.1, 60, 100)["rtpStep"]["errors"] == 1);
}

TEST_CASE("ST 2022-6: Header lesen") {
    const auto p = hbrmtPacket(1, 270000, false, 7);
    HbrmtHeader h;
    REQUIRE(parseHbrmtHeader(p.data() + 12, p.size() - 12, h));
    CHECK(h.f);
    CHECK(h.frCount == 7);
    CHECK(h.r == 2);
    CHECK(h.cf == 1);
    CHECK(h.frame == 0x21);
    CHECK(h.frate == 0x12);
    CHECK(h.sample == 0x01);
    CHECK(h.videoTimestamp == 270000);
    CHECK(h.length == 12);
    CHECK_FALSE(parseHbrmtHeader(p.data() + 12, 6, h));
}

TEST_CASE("ST 2022-6: Datagramme je Bild nach Abschnitt 6.5") {
    // 1080p50 4:2:2 10 Bit: (2640 × 10 × 2 / 8) × 1125 = 7 425 000 Oktette → 7 425 000 / 1376 + 1
    CHECK(hbrmtExpectedDatagrams(0x21, 0x12, 0x01) == 5397);
    // 1080p60: (2200 × 10 × 2 / 8) × 1125 = 6 187 500 Oktette → 6 187 500 / 1376 + 1
    CHECK(hbrmtExpectedDatagrams(0x21, 0x10, 0x01) == 4497);
    CHECK(hbrmtExpectedDatagrams(0x99, 0x12, 0x01) == 0);
    CHECK(hbrmtExpectedDatagrams(0x21, 0x12, 0x09) == 0);
    CHECK(hbrmtFrameRateHz(0x11) == doctest::Approx(59.94).epsilon(0.001));
    CHECK(hbrmtClockHz(1) == 27e6);
}

TEST_CASE("ST 2022-6: Strom ohne Fehler, danach Paketverlust") {
    HbrmtAnalyzer a;
    uint16_t seq = 100;
    uint32_t ts = 5000;
    uint8_t fr = 0;
    const uint32_t dpf = hbrmtExpectedDatagrams(0x21, 0x12, 0x01);
    double t = 0;
    for (int frame = 0; frame < 3; ++frame) {
        for (uint32_t i = 0; i < dpf; ++i) {
            const auto p = hbrmtPacket(seq++, ts + i * 100, i + 1 == dpf, fr);
            a.addPacket(p.data(), p.size(), t);
            t += 0.00001;
        }
        ts += 540000; // 27 MHz / 50 Hz
        ++fr;
    }
    auto j = a.toJson(t, 60, 100);
    CHECK(j["frames"] == 3);
    CHECK(j["lost"] == 0);
    CHECK(j["verdict"] == "ok");
    CHECK(j["datagramsPerFrame"]["last"] == dpf);
    CHECK(j["datagramsPerFrame"]["expected"] == dpf);
    CHECK(j["header"]["frameName"] == "1920×1080 progressiv");
    CHECK(j["header"]["rateName"] == "50 Hz");
    CHECK(j["header"]["referenceName"] == "UTC-synchron");
    CHECK(j["rtpStep"]["min"] == 540000);
    CHECK(j["rtpStep"]["max"] == 540000);
    CHECK(j["videoTimestampOffset"]["min"] == 0);
    // Paket fehlt
    seq += 3;
    const auto p = hbrmtPacket(seq, ts, false, fr);
    a.addPacket(p.data(), p.size(), t);
    j = a.toJson(t, 60, 100);
    CHECK(j["lost"] == 3);
    CHECK(j["verdict"] == "failed");
}

TEST_CASE("ST 2022-6: falsche Nutzlast, F-Bit und FRCount") {
    HbrmtAnalyzer a;
    auto p = hbrmtPacket(1, 0, true, 0, 0x21, 0x12, 0x01, 1, 1000);
    a.addPacket(p.data(), p.size(), 0.0);
    CHECK(a.toJson(0.1, 60, 10)["badSize"] == 1);
    p = hbrmtPacket(2, 540000, true, 5); // FRCount springt von 0 auf 5
    a.addPacket(p.data(), p.size(), 0.1);
    CHECK(a.toJson(0.2, 60, 10)["frCountErrors"] == 1);
    const uint8_t shortPacket[8] = {0x80};
    a.addPacket(shortPacket, sizeof shortPacket, 0.2);
    CHECK(a.toJson(0.3, 60, 10)["badRtp"] == 1);
}

TEST_CASE("Flow-Angaben für ANC und ST 2022-6") {
    FlowConfig anc;
    std::string error;
    REQUIRE(parseFlowSpec(Media::Anc, "239.1.1.5:20000,format=1920x1080p25,pt=100,name=Daten", anc, error));
    REQUIRE(anc.anc);
    CHECK(anc.anc->format.rate == FrameRate::P25);
    CHECK(std::string(anc.kind()) == "anc");
    CHECK(anc.formatName() == "ANC 1920x1080p25");
    FlowConfig h;
    REQUIRE(parseFlowSpec(Media::Hbrmt, "239.1.1.6:5004,red=239.2.1.6:5004,pt=98", h, error));
    REQUIRE(h.hbrmt);
    CHECK(h.hbrmt->redundant.has_value());
    CHECK(std::string(h.kind()) == "hbrmt");
    std::vector<std::string> errors, warnings;
    validateFlow(h, errors, warnings);
    CHECK(errors.empty());
    CHECK_FALSE(parseFlowSpec(Media::Hbrmt, "239.1.1.6:5004,ptime=1", h, error));
}

TEST_CASE("Flows aus der SDP: smpte291 und SMPTE2022-6") {
    const std::string sdp =
        "v=0\r\no=- 1 1 IN IP4 10.0.0.1\r\ns=Studio\r\nt=0 0\r\n"
        "m=video 20000 RTP/AVP 100\r\nc=IN IP4 239.1.1.5/32\r\na=rtpmap:100 smpte291/90000\r\n"
        "a=fmtp:100 DID_SDID={0x61,0x01};\r\n"
        "m=video 5004 RTP/AVP 98\r\nc=IN IP4 239.1.1.6/32\r\na=rtpmap:98 SMPTE2022-6/27000000\r\n";
    std::vector<std::string> errors;
    const auto flows = flowsFromSdp(sdp, errors);
    REQUIRE(flows.size() == 2);
    CHECK(flows[0].anc.has_value());
    CHECK(flows[0].anc->payloadType == 100);
    CHECK(flows[1].hbrmt.has_value());
    CHECK(flows[1].hbrmt->payloadType == 98);
}

TEST_CASE("Hub: ANC- und ST-2022-6-Flows liefern Urteil, Snapshot und Systemprüfung") {
    HubConfig c;
    c.demo = true;
    c.maxFlows = 4;
    MonitorHub hub(c);
    FlowConfig anc;
    std::string error;
    REQUIRE(parseFlowSpec(Media::Anc, "239.1.1.5:20000,format=1920x1080p50", anc, error));
    auto a = hub.addFlow(anc, error);
    REQUIRE(a);
    CHECK(a->id() == "d1");
    FlowConfig h;
    REQUIRE(parseFlowSpec(Media::Hbrmt, "239.1.1.6:5004", h, error));
    auto b = hub.addFlow(h, error);
    REQUIRE(b);
    CHECK(b->id() == "h1");

    AncPacket p;
    p.did = 0x60;
    p.sdid = 0x60;
    p.line = 9;
    p.udwSize = 16;
    a->onAncFrame(ancFrame(1000, 0, {p}));
    auto snap = hub.snapshot("d1");
    CHECK(snap["flow"]["kind"] == "anc");
    CHECK(snap["flow"]["anc"]["ancPackets"] == 1);
    CHECK(snap["flow"]["anc"]["data"][0]["name"] == "Zeitcode ATC (ST 12-2)");
    CHECK(snap["flow"].contains("system10"));
    CHECK(snap["flows"][0]["verdict"]["P"] == "narrow");

    const auto packet = hbrmtPacket(1, 0, true, 0);
    b->onHbrmtPacket(packet.data(), packet.size());
    snap = hub.snapshot("h1");
    CHECK(snap["flow"]["kind"] == "hbrmt");
    CHECK(snap["flow"]["hbrmt"]["packets"] == 1);
    CHECK(snap["flow"]["hbrmt"]["header"]["frame"] == 0x21);
    CHECK(snap["flow"]["system10"]["checks"].size() >= 3);
}
