#include "core/FlowTable.h"
#include "core/Options.h"
#include "core/PixelFormat.h"

#include <algorithm>
#include <doctest/doctest.h>

using namespace st2110;

namespace {

// Beobachtet einige Sekunden eines synthetischen ST-2110-20-Stroms (25 Hz) im angegebenen Pixelformat.
DiscoveredFlow observeStream(const PixelFormat& pf, uint32_t width, uint32_t height, bool gpm = false) {
    FlowTable table;
    const uint32_t group = pf.pgroupBytes();
    const uint32_t bytesPerPacket = std::max<uint32_t>(1, 1200 / group) * group;
    const uint32_t pixelsPerPacket = bytesPerPacket / group * pf.pgroupPixels();
    double now = 0;
    uint16_t seq = 0;
    for (int frame = 0; frame < 60; ++frame) {
        PacketInfo p;
        p.srcIp = 0x0A000001;
        p.dstIp = (239u << 24) | 0x640001;
        p.srcPort = 10000;
        p.dstPort = 20000;
        p.wireBytes = bytesPerPacket + 62;
        p.rtp = true;
        p.payloadType = 96;
        p.ssrc = 7;
        p.timestamp = static_cast<uint32_t>(frame) * 3600;
        p.rtpPayload = static_cast<uint16_t>(bytesPerPacket + 8);
        p.rfc4175 = true;
        p.videoBytes = bytesPerPacket;
        p.segments = 1;
        p.firstLength = static_cast<uint16_t>(bytesPerPacket);
        p.lastLength = p.firstLength;
        for (uint32_t k = 0; k < 12; ++k) {
            p.sequence = seq++;
            p.row = 0;
            p.rowOffset = static_cast<uint16_t>(k * pixelsPerPacket);
            p.lastRow = p.row;
            p.lastOffset = p.rowOffset;
            table.observe(p, now);
            now += 0.0005;
        }
        if (gpm) {
            p.segments = 2;
            p.sequence = seq++;
            table.observe(p, now);
            p.segments = 1;
        }
        p.sequence = seq++;
        p.row = static_cast<uint16_t>(height - 1);
        p.rowOffset = static_cast<uint16_t>(width - pixelsPerPacket);
        p.lastRow = p.row;
        p.lastOffset = p.rowOffset;
        p.marker = true;
        table.observe(p, now);
        now += 0.04;
    }
    const auto list = table.list(now);
    REQUIRE(list.size() == 1);
    return list[0];
}

} // namespace

TEST_CASE("Erkennung: alle 16 Pixelformate von ST 2110-20 mit Breite, Höhe und Rate") {
    for (const auto& pf : allPixelFormats()) {
        const DiscoveredFlow f = observeStream(pf, 1920, 1080);
        INFO(pf.name());
        CHECK(f.kind == "video");
        REQUIRE(f.video.has_value());
        CHECK(f.video->width == 1920);
        CHECK(f.video->height == 1080);
        CHECK(f.video->rate == FrameRate::P25);
        // Der Kandidat mit der passenden Pixelgruppe ist dabei; gleiche Pixelgruppen sind nicht zu trennen.
        CHECK(std::find(f.pixelCandidates.begin(), f.pixelCandidates.end(), pf) != f.pixelCandidates.end());
        for (const auto& c : f.pixelCandidates) {
            CHECK(c.bytesPerPixel() == doctest::Approx(pf.bytesPerPixel()));
        }
    }
}

TEST_CASE("Erkennung: eindeutige Formate und mehrdeutige Formate gleicher Pixelgruppe") {
    CHECK(observeStream({Sampling::Ycbcr422, 10}, 1280, 720).pixelCandidates.size() == 1);
    CHECK(observeStream({Sampling::Ycbcr420, 10}, 1920, 1080).pixelCandidates.size() == 1);
    // RGB 12 Bit und YCbCr 4:4:4 12 Bit teilen sich die Pixelgruppe (9 Byte für 2 Bildpunkte)
    const auto ambiguous = observeStream({Sampling::Rgb, 12}, 1920, 1080);
    CHECK(ambiguous.pixelCandidates.size() == 2);
    // 3 Byte je Bildpunkt: vier Formate (RGB 8, 4:4:4 8, 4:2:2 12, 4:2:0 16 Bit)
    CHECK(observeStream({Sampling::Rgb, 8}, 1920, 1080).pixelCandidates.size() == 4);
}

TEST_CASE("Erkennung: Packmodus GPM nur sicher bei mehreren Zeilensegmenten") {
    const auto bpm = observeStream({Sampling::Ycbcr422, 10}, 1920, 1080, false);
    REQUIRE(bpm.packing.has_value());
    CHECK(*bpm.packing == Packing::Bpm);
    CHECK_FALSE(bpm.packingCertain);
    const auto gpm = observeStream({Sampling::Ycbcr422, 10}, 1920, 1080, true);
    REQUIRE(gpm.packing.has_value());
    CHECK(*gpm.packing == Packing::Gpm);
    CHECK(gpm.packingCertain);
}

TEST_CASE("Flow-Angabe mit Abtastung, Tiefe und Packmodus; Breite muss zur Pixelgruppe passen") {
    FlowConfig f;
    std::string error;
    REQUIRE(parseFlowSpec(Media::Video,
                          "239.1.1.1:20000,format=3840x2160p50,sampling=RGB,depth=10,packing=GPM", f, error));
    CHECK(f.video->format.pixel == PixelFormat{Sampling::Rgb, 10});
    CHECK(f.video->packing == Packing::Gpm);
    CHECK(f.formatName() == "3840x2160p50 RGB 10 Bit");
    std::vector<std::string> errors, warnings;
    validateFlow(f, errors, warnings);
    CHECK(errors.empty());
    // 1366 ist kein Vielfaches von 4 (RGB 10 Bit: 4 Bildpunkte je Pixelgruppe)
    REQUIRE(
        parseFlowSpec(Media::Video, "239.1.1.1:20000,format=1366x768p50,sampling=RGB,depth=10", f, error));
    validateFlow(f, errors, warnings);
    CHECK_FALSE(errors.empty());
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1:20000,depth=14", f, error));
    CHECK_FALSE(parseFlowSpec(Media::Video, "239.1.1.1:20000,sampling=XYZ", f, error));
    // Reihenfolge der Schlüssel spielt keine Rolle
    REQUIRE(parseFlowSpec(Media::Video, "239.1.1.1:20000,sampling=YCbCr-4:2:0,format=1920x1080p25,depth=8", f,
                          error));
    CHECK(f.video->format.pixel == PixelFormat{Sampling::Ycbcr420, 8});
}
