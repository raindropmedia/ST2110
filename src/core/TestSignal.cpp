#include "core/TestSignal.h"

#include "core/Preview.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace st2110 {
namespace {

void packPair(uint8_t* p, const Ycbcr10& a, const Ycbcr10& b) {
    // Cb und Cr aus dem Mittel beider Bildpunkte (4:2:2)
    const uint16_t cb = static_cast<uint16_t>((a.cb + b.cb + 1) / 2);
    const uint16_t cr = static_cast<uint16_t>((a.cr + b.cr + 1) / 2);
    const uint16_t y0 = a.y;
    const uint16_t y1 = b.y;
    p[0] = static_cast<uint8_t>(cb >> 2);
    p[1] = static_cast<uint8_t>(((cb & 0x3) << 6) | (y0 >> 4));
    p[2] = static_cast<uint8_t>(((y0 & 0xF) << 4) | (cr >> 6));
    p[3] = static_cast<uint8_t>(((cr & 0x3F) << 2) | (y1 >> 8));
    p[4] = static_cast<uint8_t>(y1 & 0xFF);
}

// Wandelt eine RGB-Zeile (width Bildpunkte) in eine RFC-4175-Zeile.
std::vector<uint8_t> packLine(const uint8_t* rgb, uint32_t width) {
    std::vector<uint8_t> line(static_cast<size_t>(width / 2) * 5);
    for (uint32_t x = 0; x + 1 < width; x += 2) {
        const uint8_t* p = rgb + static_cast<size_t>(x) * 3;
        packPair(line.data() + static_cast<size_t>(x / 2) * 5, rgbToYcbcr709(p[0], p[1], p[2]),
                 rgbToYcbcr709(p[3], p[4], p[5]));
    }
    return line;
}

} // namespace

Ycbcr10 rgbToYcbcr709(uint8_t r, uint8_t g, uint8_t b) {
    const double rn = r / 255.0;
    const double gn = g / 255.0;
    const double bn = b / 255.0;
    const double y = 0.2126 * rn + 0.7152 * gn + 0.0722 * bn;
    const double pb = (bn - y) / 1.8556;
    const double pr = (rn - y) / 1.5748;
    const auto clamp10 = [](double v, int lo, int hi) {
        return static_cast<uint16_t>(std::clamp(static_cast<int>(std::lround(v)), lo, hi));
    };
    return Ycbcr10{clamp10(64 + 876 * y, 64, 940), clamp10(512 + 896 * pb, 64, 960),
                   clamp10(512 + 896 * pr, 64, 960)};
}

TestPatternGenerator::TestPatternGenerator(uint32_t width, uint32_t height, bool interlaced)
    : width_(width & ~1u), height_(height), interlaced_(interlaced),
      lineBytes_(static_cast<size_t>(width_ / 2) * 5) {
    // Gleiche Aufteilung wie renderTestPattern(): Balken bis 2/3, Treppe bis 5/6, darunter dunkel.
    barsEnd_ = height_ * 2 / 3;
    rampEnd_ = height_ * 5 / 6;
    box_ = std::max<uint32_t>(4, height_ / 6) & ~1u;
    boxY_ = barsEnd_ + (rampEnd_ - barsEnd_ - std::min(box_, rampEnd_ - barsEnd_)) / 2;

    // Musterzeilen aus der Vorschau-Funktion holen (ein Bild mit kleiner Höhe genügt nicht, weil die
    // Bänder relativ zur Höhe liegen; deshalb je Band eine Zeile des vollen Bildes rendern).
    PreviewImage image;
    renderTestPattern(image, width_, height_, 0.5, 0, false); // t = 0,5: kein Blitz, Quadrat in der Mitte
    const auto rowOf = [&](uint32_t y) { return image.rgb.data() + static_cast<size_t>(y) * width_ * 3; };
    barsLine_ = packLine(rowOf(0), width_);
    // Treppe: eine Zeile oberhalb des Quadrats
    rampLine_ = packLine(rowOf(barsEnd_), width_);
    bottomLine_ = packLine(rowOf(height_ - 1), width_);
    PreviewImage flash;
    renderTestPattern(flash, width_, height_, 0.0, 0, false); // t = 0: Blitz an
    flashLine_ = packLine(flash.rgb.data() + static_cast<size_t>(height_ - 1) * width_ * 3, width_);
    boxPair_.resize(5);
    const Ycbcr10 c = rgbToYcbcr709(235, 64, 32);
    packPair(boxPair_.data(), c, c);
}

TestPatternGenerator::Band TestPatternGenerator::bandOf(uint32_t line) const {
    if (line < barsEnd_) {
        return Band::Bars;
    }
    return line < rampEnd_ ? Band::Ramp : Band::Bottom;
}

void TestPatternGenerator::render(uint8_t* dst, size_t linesize, double t, bool secondField) const {
    const bool flash = std::fmod(t, 1.0) < 0.08;
    const double phase = std::fmod(t * 0.25, 1.0);
    const uint32_t boxX = static_cast<uint32_t>(phase * (width_ - box_)) & ~1u;
    const uint32_t lines = linesPerFrame();
    for (uint32_t i = 0; i < lines; ++i) {
        const uint32_t y = interlaced_ ? 2 * i + (secondField ? 1 : 0) : i;
        uint8_t* row = dst + i * linesize;
        switch (bandOf(y)) {
        case Band::Bars:
            std::memcpy(row, barsLine_.data(), lineBytes_);
            break;
        case Band::Ramp:
            std::memcpy(row, rampLine_.data(), lineBytes_);
            break;
        case Band::Bottom:
            std::memcpy(row, (flash ? flashLine_ : bottomLine_).data(), lineBytes_);
            break;
        }
        if (y >= boxY_ && y < boxY_ + box_) {
            for (uint32_t x = boxX; x < boxX + box_ && x + 1 < width_; x += 2) {
                std::memcpy(row + static_cast<size_t>(x / 2) * 5, boxPair_.data(), 5);
            }
        }
    }
}

void renderTestTonePcm(uint8_t* dst, uint64_t sampleIndex, size_t frames, uint16_t channels,
                       uint32_t sampleRate, uint8_t bitDepth) {
    std::vector<float> samples;
    generateTestTone(samples, sampleIndex, frames, channels, sampleRate);
    const double scale = bitDepth == 16 ? 32767.0 : 8388607.0;
    const size_t bytes = bitDepth == 16 ? 2 : 3;
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto v = static_cast<int32_t>(std::lround(std::clamp(samples[i], -1.0f, 1.0f) * scale));
        const auto u = static_cast<uint32_t>(v);
        uint8_t* p = dst + i * bytes;
        if (bytes == 2) {
            p[0] = static_cast<uint8_t>(u >> 8);
            p[1] = static_cast<uint8_t>(u);
        } else {
            p[0] = static_cast<uint8_t>(u >> 16);
            p[1] = static_cast<uint8_t>(u >> 8);
            p[2] = static_cast<uint8_t>(u);
        }
    }
}

} // namespace st2110
