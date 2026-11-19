#include "core/Preview.h"

#include <cmath>
#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

namespace {

// Ein Bild in RFC 4175 (4:2:2, 10 Bit) mit einer Farbe je Hälfte erzeugen.
std::vector<uint8_t> makeFrame(uint32_t width, uint32_t lines, int yLeft, int yRight, int cb, int cr) {
    std::vector<uint8_t> f(static_cast<size_t>(width) / 2 * 5 * lines);
    for (uint32_t l = 0; l < lines; ++l) {
        for (uint32_t p = 0; p < width / 2; ++p) {
            const int y = p < width / 4 ? yLeft : yRight;
            uint8_t* g = f.data() + (static_cast<size_t>(l) * (width / 2) + p) * 5;
            const uint64_t bits = (static_cast<uint64_t>(cb) << 30) | (static_cast<uint64_t>(y) << 20) |
                                  (static_cast<uint64_t>(cr) << 10) | static_cast<uint64_t>(y);
            for (int i = 0; i < 5; ++i) {
                g[i] = static_cast<uint8_t>(bits >> (8 * (4 - i)));
            }
        }
    }
    return f;
}

} // namespace

TEST_CASE("Vorschau: RFC 4175 nach RGB, Verkleinerung und Farben") {
    // links Weiß (Y 940), rechts Schwarz (Y 64), ohne Farbe (Cb = Cr = 512)
    auto frame = makeFrame(1920, 1080, 940, 64, 512, 512);
    PreviewImage img;
    REQUIRE(convertRfc4175ToRgb(frame.data(), frame.size(), 1920, 1080, false, 480, img));
    CHECK(img.width == 480);
    CHECK(img.height == 270);
    CHECK(img.rgb.size() == 480u * 270u * 3u);
    CHECK(img.rgb[0] == 255);
    CHECK(img.rgb[1] == 255);
    CHECK(img.rgb[2] == 255);
    const size_t right = (static_cast<size_t>(10) * 480 + 400) * 3;
    CHECK(img.rgb[right] == 0);

    // Halbbild 1080i: 540 Zeilen ergeben ebenfalls 270
    auto field = makeFrame(1920, 540, 940, 64, 512, 512);
    REQUIRE(convertRfc4175ToRgb(field.data(), field.size(), 1920, 540, true, 480, img));
    CHECK(img.height == 270);

    // 720p: gerade Schrittweite, Seitenverhältnis bleibt 16:9
    auto hd = makeFrame(1280, 720, 940, 64, 512, 512);
    REQUIRE(convertRfc4175ToRgb(hd.data(), hd.size(), 1280, 720, false, 480, img));
    CHECK(img.width == 320);
    CHECK(img.height == 180);

    // reines Rot (BT.709: Y 250, Cb 409, Cr 960 bei 10 Bit, begrenzter Bereich)
    auto red = makeFrame(64, 4, 250, 250, 409, 960);
    REQUIRE(convertRfc4175ToRgb(red.data(), red.size(), 64, 4, false, 480, img));
    CHECK(img.width == 64);
    CHECK(img.rgb[0] > 240);
    CHECK(img.rgb[1] < 15);
    CHECK(img.rgb[2] < 15);

    CHECK_FALSE(convertRfc4175ToRgb(frame.data(), 100, 1920, 1080, false, 480, img));
}

TEST_CASE("Vorschau: Bilder nur auf Anforderung und im Abstand") {
    VideoPreview v;
    CHECK_FALSE(v.due(0.0));
    v.request(0.0);
    CHECK(v.due(0.0));
    PreviewImage img;
    renderTestPattern(img, 160, 90, 0.5, 1, false);
    CHECK(img.rgb.size() == 160u * 90u * 3u);
    v.put(img, 0.0);
    CHECK_FALSE(v.due(0.1));
    CHECK(v.due(0.25));
    CHECK_FALSE(v.due(10.0)); // Anforderung abgelaufen
    CHECK(v.latest().sequence == 1);
    CHECK(v.latest().width == 160);
}

TEST_CASE("Vorschau: Ton-Ringpuffer, Kanalpaar und Pegel") {
    AudioPreview a;
    a.configure(48000, 4);
    // 24 Bit Big Endian: Kanal 1 halbe Aussteuerung, Kanal 3 negativ voll
    std::vector<uint8_t> pcm;
    for (int f = 0; f < 480; ++f) {
        const int32_t values[4] = {0x400000, 0, -0x800000, 0x100000};
        for (int32_t v : values) {
            const auto u = static_cast<uint32_t>(v) & 0xFFFFFF;
            pcm.push_back(static_cast<uint8_t>(u >> 16));
            pcm.push_back(static_cast<uint8_t>(u >> 8));
            pcm.push_back(static_cast<uint8_t>(u));
        }
    }
    a.pushPcm(pcm.data(), pcm.size(), 24);
    auto c = a.read(0, 0, 48000);
    CHECK(c.channels == 4);
    CHECK(c.sampleRate == 48000);
    CHECK(c.frames == 480);
    CHECK(c.start == 0);
    CHECK(c.stereo[0] == 16383);
    CHECK(c.stereo[1] == 0);
    CHECK(c.peaksDbfs[0] == doctest::Approx(-6.02).epsilon(0.01));
    CHECK(c.peaksDbfs[2] == doctest::Approx(0.0).epsilon(0.01));
    auto pair2 = a.read(c.start, 1, 48000);
    CHECK(pair2.stereo[0] == -32767);
    CHECK(pair2.peaksDbfs[0] == -120.0f); // seit der letzten Abfrage nichts Neues
    auto none = a.read(480, 0, 48000);
    CHECK(none.frames == 0);
}

TEST_CASE("Vorschau: Testton mit EBU-Kennung") {
    std::vector<float> tone;
    generateTestTone(tone, 0, 48000, 2, 48000);
    float maxL = 0, maxR = 0;
    for (size_t i = 0; i < 48000; ++i) {
        if (i < 12000) {
            CHECK(tone[i * 2] == 0.0f); // erste 250 ms links still
        }
        maxL = std::max(maxL, std::fabs(tone[i * 2]));
        maxR = std::max(maxR, std::fabs(tone[i * 2 + 1]));
    }
    CHECK(20.0 * std::log10(maxR) == doctest::Approx(-18.0).epsilon(0.01));
    CHECK(maxL > 0.1f);
}
