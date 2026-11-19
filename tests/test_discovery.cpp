#include "core/FlowTable.h"
#include "core/MonitorHub.h"
#include "core/PacketParser.h"
#include "core/SapDirectory.h"

#include <cstring>
#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

namespace {

void put16(std::vector<uint8_t>& b, size_t at, uint16_t v) {
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}

void put32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    put16(b, at, static_cast<uint16_t>(v >> 16));
    put16(b, at + 2, static_cast<uint16_t>(v));
}

// Ethernet (optional VLAN) + IPv4 + UDP + RTP; payload: RTP-Nutzlast (nur Kopf vorhanden, Länge aus UDP).
std::vector<uint8_t> frame(uint32_t src, uint32_t dst, uint16_t dport, uint8_t pt, bool marker, uint16_t seq,
                           uint32_t ts, const std::vector<uint8_t>& rtpPayloadHead, uint16_t rtpPayloadLength,
                           bool vlan = false) {
    const size_t eth = vlan ? 18 : 14;
    std::vector<uint8_t> b(eth + 20 + 8 + 12 + rtpPayloadHead.size(), 0);
    if (vlan) {
        put16(b, 12, 0x8100);
        put16(b, 14, 100);
        put16(b, 16, 0x0800);
    } else {
        put16(b, 12, 0x0800);
    }
    const size_t ip = eth;
    b[ip] = 0x45;
    b[ip + 1] = 34 << 2; // DSCP AF41
    const uint16_t udpLength = static_cast<uint16_t>(8 + 12 + rtpPayloadLength);
    put16(b, ip + 2, static_cast<uint16_t>(20 + udpLength));
    b[ip + 8] = 32;
    b[ip + 9] = 17;
    put32(b, ip + 12, src);
    put32(b, ip + 16, dst);
    const size_t udp = ip + 20;
    put16(b, udp, 10000);
    put16(b, udp + 2, dport);
    put16(b, udp + 4, udpLength);
    const size_t rtp = udp + 8;
    b[rtp] = 0x80;
    b[rtp + 1] = static_cast<uint8_t>((marker ? 0x80 : 0) | pt);
    put16(b, rtp + 2, seq);
    put32(b, rtp + 4, ts);
    put32(b, rtp + 8, 0x12345678);
    std::memcpy(b.data() + rtp + 12, rtpPayloadHead.data(), rtpPayloadHead.size());
    return b;
}

std::vector<uint8_t> rfc4175Head(uint16_t row, bool secondField, uint16_t offset, uint16_t length) {
    std::vector<uint8_t> h(8, 0);
    put16(h, 2, length);
    put16(h, 4, static_cast<uint16_t>((secondField ? 0x8000 : 0) | row));
    put16(h, 6, offset);
    return h;
}

constexpr uint32_t kSrc = (10u << 24) | 5;                   // 10.0.0.5
constexpr uint32_t kVideo = (239u << 24) | (100u << 16) | 1; // 239.100.0.1
constexpr uint32_t kAudio = (239u << 24) | (100u << 16) | 2;

// 1080i50: je Halbbild 540 Zeilen zu je 4 Paketen mit 1200 Byte, Marker am Ende des Halbbilds.
void feedInterlaced(FlowTable& table, double seconds, bool withLoss = false) {
    uint16_t seq = 0;
    uint32_t ts = 0;
    double t = 0;
    const int fields = static_cast<int>(seconds * 50);
    for (int field = 0; field < fields; ++field) {
        const bool second = field % 2 == 1;
        for (uint16_t row = 0; row < 540; ++row) {
            for (uint16_t k = 0; k < 4; ++k) {
                const bool marker = row == 539 && k == 3;
                PacketInfo p;
                auto bytes = frame(kSrc, kVideo, 20000, 96, marker, seq++, ts,
                                   rfc4175Head(row, second, k * 480, 1200), 8 + 1200);
                if (withLoss && field == 3 && row == 10 && k == 0) {
                    continue; // ein Paket fehlt
                }
                REQUIRE(parseEthernetFrame(bytes.data(), bytes.size(), 1262, p));
                table.observe(p, t);
                t += 0.02 / 2160.0;
            }
        }
        ts += 1800;
    }
}

} // namespace

TEST_CASE("Paketparser: IPv4, UDP, RTP und RFC 4175") {
    auto bytes =
        frame(kSrc, kVideo, 20000, 96, true, 7, 1800, rfc4175Head(539, true, 1440, 1200), 1208, true);
    PacketInfo p;
    REQUIRE(parseEthernetFrame(bytes.data(), bytes.size(), 1266, p));
    CHECK(ipv4ToString(p.srcIp) == "10.0.0.5");
    CHECK(ipv4ToString(p.dstIp) == "239.100.0.1");
    CHECK(p.dstPort == 20000);
    CHECK(p.vlan == 100);
    CHECK(p.dscp == 34);
    CHECK(p.wireBytes == 1266);
    CHECK(p.rtp);
    CHECK(p.payloadType == 96);
    CHECK(p.marker);
    CHECK(p.sequence == 7);
    CHECK(p.timestamp == 1800);
    CHECK(p.ssrc == 0x12345678);
    CHECK(p.rtpPayload == 1208);
    CHECK(p.rfc4175);
    CHECK(p.row == 539);
    CHECK(p.secondField);
    CHECK(p.rowOffset == 1440);
    CHECK(p.videoBytes == 1200);

    // Ton: Nutzlast ohne Zeilenköpfe
    auto audio = frame(kSrc, kAudio, 30000, 97, false, 1, 48, std::vector<uint8_t>(8, 0x55), 288);
    REQUIRE(parseEthernetFrame(audio.data(), audio.size(), 342, p));
    CHECK(p.rtp);
    CHECK_FALSE(p.rfc4175);
    CHECK(p.rtpPayload == 288);

    // kein IPv4
    std::vector<uint8_t> arp(60, 0);
    put16(arp, 12, 0x0806);
    CHECK_FALSE(parseEthernetFrame(arp.data(), arp.size(), 60, p));
}

TEST_CASE("Erkennung: 1080i50 aus RFC-4175-Köpfen und RTP-Zeitstempeln") {
    FlowTable table;
    feedInterlaced(table, 1.2);
    auto list = table.list(1.2);
    REQUIRE(list.size() == 1);
    const auto& f = list[0];
    CHECK(f.kind == "video");
    CHECK(f.clockRate == 90000);
    REQUIRE(f.video);
    CHECK(f.formatGuess() == "1920x1080i50");
    CHECK(f.pps == doctest::Approx(108000).epsilon(0.02));
    CHECK(f.bps == doctest::Approx(108000 * 1262 * 8.0).epsilon(0.02));
    CHECK(f.sequenceErrors == 0);
    auto json = discoveredFlowToJson(f, 1.2, "v1");
    CHECK(json["dstIp"] == "239.100.0.1");
    CHECK(json["multicast"] == true);
    CHECK(json["monitored"] == "v1");
    CHECK(json["active"] == true);
}

TEST_CASE("Erkennung: Sequenzlücken werden gezählt, alte Flows verfallen") {
    FlowTable table;
    feedInterlaced(table, 0.2, true);
    CHECK(table.list(0.2)[0].sequenceErrors == 1);
    CHECK(table.list(5.0)[0].pps == 0); // inaktiv
    table.expire(100.0);
    CHECK(table.size() == 0);
}

TEST_CASE("Erkennung: Ton L24/48000/2 mit 1 ms und PTP") {
    FlowTable table;
    for (int i = 0; i < 1500; ++i) {
        auto bytes = frame(kSrc, kAudio, 30000, 97, false, static_cast<uint16_t>(i),
                           static_cast<uint32_t>(i * 48), std::vector<uint8_t>(8, 0), 288);
        PacketInfo p;
        REQUIRE(parseEthernetFrame(bytes.data(), bytes.size(), 342, p));
        table.observe(p, i * 0.001);
    }
    PacketInfo ptp;
    ptp.dstIp = (224u << 24) | 129;
    ptp.dstPort = 319;
    ptp.wireBytes = 86;
    table.observe(ptp, 1.4);
    auto list = table.list(1.5);
    REQUIRE(list.size() == 2);
    const auto& a = list[1].dstPort == 30000 ? list[1] : list[0];
    const auto& p = list[1].dstPort == 30000 ? list[0] : list[1];
    CHECK(a.kind == "audio");
    CHECK(a.formatGuess() == "L24/48000/2 1ms");
    CHECK(p.kind == "ptp");
}

TEST_CASE("Erkennung: zusammengefasste Pakete (Demo) ergeben dieselben Raten") {
    FlowTable table;
    PacketInfo p;
    p.dstIp = kVideo;
    p.dstPort = 20000;
    p.srcIp = kSrc;
    p.wireBytes = 1262;
    p.rtp = true;
    for (int i = 0; i <= 60; ++i) {
        table.observe(p, i * 0.02, 4320);
    }
    CHECK(table.list(1.2)[0].pps == doctest::Approx(216000).epsilon(0.03));
}

TEST_CASE("SAP: Ankündigung, Wiederholung und Löschung") {
    const std::string sdp =
        "v=0\no=- 1 1 IN IP4 10.0.0.5\ns=Kamera 3\nt=0 0\nm=video 20000 RTP/AVP 96\n"
        "c=IN IP4 239.100.0.21/64\na=rtpmap:96 raw/90000\n"
        "a=fmtp:96 sampling=YCbCr-4:2:2; width=1280; height=720; exactframerate=50; depth=10\n";
    std::vector<uint8_t> packet{0x20, 0, 0x12, 0x34, 10, 0, 0, 5};
    const std::string type = "application/sdp";
    packet.insert(packet.end(), type.begin(), type.end());
    packet.push_back(0);
    packet.insert(packet.end(), sdp.begin(), sdp.end());

    SapMessage m;
    std::string error;
    REQUIRE(parseSapPacket(packet.data(), packet.size(), m, error));
    CHECK_FALSE(m.deletion);
    CHECK(m.origin == "10.0.0.5");
    CHECK(m.hash == 0x1234);

    SapDirectory dir;
    dir.add(m, 0);
    dir.add(m, 30);
    REQUIRE(dir.entries().size() == 1);
    const auto& e = dir.entries().begin()->second;
    CHECK(e.name == "Kamera 3");
    CHECK(e.announcements == 2);
    REQUIRE(e.flows.size() == 1);
    CHECK(e.flows[0].formatName() == "1280x720p50");

    packet[0] |= 0x04; // Löschung
    REQUIRE(parseSapPacket(packet.data(), packet.size(), m, error));
    CHECK(m.deletion);
    dir.add(m, 40);
    CHECK(dir.entries().empty());

    std::vector<uint8_t> encrypted{0x22, 0, 0, 1, 10, 0, 0, 5};
    CHECK_FALSE(parseSapPacket(encrypted.data(), encrypted.size(), m, error));
}

TEST_CASE("Hub: erkannte Flows werden überwachten Flows zugeordnet") {
    MonitorHub hub(HubConfig{});
    std::string error;
    FlowConfig f;
    parseFlowSpec(Media::Video, "239.100.0.1:20000", f, error);
    REQUIRE(hub.addFlow(f, error));
    PacketInfo p;
    p.dstIp = kVideo;
    p.dstPort = 20000;
    p.srcIp = kSrc;
    p.wireBytes = 1262;
    hub.onDiscoveredPacket(p);
    p.dstIp = kAudio;
    hub.onDiscoveredPacket(p);
    auto json = hub.flowsJson();
    REQUIRE(json["discovered"].size() == 2);
    CHECK(json["discovered"][0]["monitored"] == "v1");
    CHECK(json["discovered"][1]["monitored"].is_null());
}
