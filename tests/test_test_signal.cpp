#include "core/Preview.h"
#include "core/TestSignal.h"

#include <cstdlib>
#include <doctest/doctest.h>
#include <vector>

using namespace st2110;

TEST_CASE("Testsignal: RGB nach YCbCr BT.709, begrenzter Bereich") {
    const auto white = rgbToYcbcr709(255, 255, 255);
    CHECK(white.y == 940);
    CHECK(white.cb == 512);
    CHECK(white.cr == 512);
    const auto black = rgbToYcbcr709(0, 0, 0);
    CHECK(black.y == 64);
    const auto red = rgbToYcbcr709(255, 0, 0);
    CHECK(red.y == 250); // 64 + 876 · 0,2126
    CHECK(red.cr == 960);
}

TEST_CASE("Testsignal: Testbild im Transportformat entspricht dem Vorschau-Testbild") {
    const uint32_t width = 1920;
    const uint32_t height = 1080;
    TestPatternGenerator gen(width, height, false);
    CHECK(gen.lineBytes() == 4800);
    CHECK(gen.linesPerFrame() == 1080);
    const size_t linesize = gen.lineBytes() + 64; // mit Füllbytes
    std::vector<uint8_t> frame(linesize * height, 0xAA);
    const double t = 1.5;
    gen.render(frame.data(), linesize, t);
    // Füllbytes bleiben unberührt
    CHECK(frame[gen.lineBytes()] == 0xAA);

    // Ohne Füllbytes zurückwandeln und mit dem RGB-Testbild vergleichen
    std::vector<uint8_t> packed(gen.lineBytes() * height);
    for (uint32_t y = 0; y < height; ++y) {
        std::copy_n(frame.data() + y * linesize, gen.lineBytes(), packed.data() + y * gen.lineBytes());
    }
    PreviewImage decoded;
    REQUIRE(convertRfc4175ToRgb(packed.data(), packed.size(), width, height, false, width, decoded));
    PreviewImage reference;
    renderTestPattern(reference, width, height, t, 0, false);
    REQUIRE(decoded.rgb.size() == reference.rgb.size());
    // Stichproben: Balken, Treppe, Quadrat, dunkle Fläche (Abweichung durch Rundung und 4:2:2 an Kanten)
    const uint32_t points[][2] = {{100, 100}, {1000, 500}, {1800, 700}, {960, 800}, {200, 1000}};
    for (const auto& pt : points) {
        const size_t i = (static_cast<size_t>(pt[1]) * width + pt[0]) * 3;
        for (int c = 0; c < 3; ++c) {
            CHECK(std::abs(decoded.rgb[i + c] - reference.rgb[i + c]) <= 3);
        }
    }
}

TEST_CASE("Testsignal: Halbbilder enthalten abwechselnd die Zeilen des Vollbilds") {
    TestPatternGenerator gen(64, 60, true);
    CHECK(gen.linesPerFrame() == 30);
    std::vector<uint8_t> f1(gen.lineBytes() * 30);
    std::vector<uint8_t> f2(gen.lineBytes() * 30);
    gen.render(f1.data(), gen.lineBytes(), 0.5, false);
    gen.render(f2.data(), gen.lineBytes(), 0.5, true);
    // Zeile 39 (Treppe) ist im zweiten Halbbild Zeile 19, Zeile 0 (Balken) im ersten Zeile 0
    TestPatternGenerator progressive(64, 60, false);
    std::vector<uint8_t> f(gen.lineBytes() * 60);
    progressive.render(f.data(), gen.lineBytes(), 0.5);
    CHECK(std::equal(f1.begin(), f1.begin() + gen.lineBytes(), f.begin()));
    CHECK(std::equal(f2.begin() + 19 * gen.lineBytes(), f2.begin() + 20 * gen.lineBytes(),
                     f.begin() + 39 * gen.lineBytes()));
}

TEST_CASE("Testsignal: Testton als L24 Big Endian") {
    std::vector<uint8_t> pcm(48 * 2 * 3);
    // ab 0,5 s (außerhalb der EBU-Pause links)
    renderTestTonePcm(pcm.data(), 24000, 48, 2, 48000, 24);
    std::vector<float> reference;
    generateTestTone(reference, 24000, 48, 2, 48000);
    // Erster Abtastwert links zurückrechnen
    const int32_t raw = (static_cast<int32_t>(pcm[0]) << 24 | pcm[1] << 16 | pcm[2] << 8) >> 8;
    CHECK(raw / 8388607.0 == doctest::Approx(reference[0]).epsilon(1e-5));
    std::vector<uint8_t> l16(48 * 2 * 2);
    renderTestTonePcm(l16.data(), 24000, 48, 2, 48000, 16);
    const auto v16 = static_cast<int16_t>(l16[2] << 8 | l16[3]);
    CHECK(v16 / 32767.0 == doctest::Approx(reference[1]).epsilon(1e-3));
}
