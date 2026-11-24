#include "core/TestSignal.h"

#include "core/Preview.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

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

namespace {

// Schreibt Werte mit fester Bitbreite, höchstwertiges Bit zuerst.
class BitWriter {
  public:
    explicit BitWriter(uint8_t* dst) : dst_(dst) {}
    void put(uint32_t value, int bits) {
        for (int i = bits - 1; i >= 0; --i) {
            if (fill_ == 0) {
                dst_[pos_] = 0;
            }
            dst_[pos_] = static_cast<uint8_t>(dst_[pos_] | (((value >> i) & 1u) << (7 - fill_)));
            if (++fill_ == 8) {
                fill_ = 0;
                ++pos_;
            }
        }
    }
    size_t bytes() const { return pos_ + (fill_ != 0 ? 1 : 0); }

  private:
    uint8_t* dst_;
    size_t pos_ = 0;
    int fill_ = 0;
};

struct Rgb8 {
    uint8_t r, g, b;
};

// Komponenten eines Bildpunkts in der Tiefe des Formats: YCbCr (begrenzter Bereich, BT.709) oder RGB (voll).
struct Comps {
    uint32_t a; // Y bzw. R
    uint32_t b; // Cb bzw. G
    uint32_t c; // Cr bzw. B
};

Comps toComps(const PixelFormat& pf, const Rgb8& p) {
    const int shift = pf.depth - 8;
    if (pf.sampling == Sampling::Rgb) {
        const uint32_t maxValue = (1u << pf.depth) - 1;
        auto scale = [&](uint8_t v) { return static_cast<uint32_t>((v * maxValue + 127) / 255); };
        return Comps{scale(p.r), scale(p.g), scale(p.b)};
    }
    const double rn = p.r / 255.0;
    const double gn = p.g / 255.0;
    const double bn = p.b / 255.0;
    const double y = 0.2126 * rn + 0.7152 * gn + 0.0722 * bn;
    const double pb = (bn - y) / 1.8556;
    const double pr = (rn - y) / 1.5748;
    const double unit = static_cast<double>(1u << shift);
    auto q = [&](double v, int lo, int hi) {
        return static_cast<uint32_t>(
            std::clamp(static_cast<int>(std::lround(v * unit)), lo << shift, hi << shift));
    };
    return Comps{q(16 + 219 * y, 16, 235), q(128 + 224 * pb, 16, 240), q(128 + 224 * pr, 16, 240)};
}

Comps average(const Comps& a, const Comps& b) {
    return Comps{(a.a + b.a + 1) / 2, (a.b + b.b + 1) / 2, (a.c + b.c + 1) / 2};
}

// Packt eine Einheitszeile: row0 (und bei 4:2:0 row1) sind RGB-Zeilen mit width Bildpunkten.
void packUnit(const PixelFormat& pf, const Rgb8* row0, const Rgb8* row1, uint32_t width, uint8_t* out) {
    BitWriter w(out);
    const int d = pf.depth;
    switch (pf.sampling) {
    case Sampling::Ycbcr422:
        for (uint32_t x = 0; x < width; x += 2) {
            const Comps a = toComps(pf, row0[x]);
            const Comps b = toComps(pf, row0[x + 1]);
            const Comps c = average(a, b);
            w.put(c.b, d);
            w.put(a.a, d);
            w.put(c.c, d);
            w.put(b.a, d);
        }
        break;
    case Sampling::Ycbcr444:
    case Sampling::Rgb:
        for (uint32_t x = 0; x < width; ++x) {
            const Comps a = toComps(pf, row0[x]);
            // YCbCr: Cb Y Cr; RGB: R G B
            if (pf.sampling == Sampling::Rgb) {
                w.put(a.a, d);
                w.put(a.b, d);
                w.put(a.c, d);
            } else {
                w.put(a.b, d);
                w.put(a.a, d);
                w.put(a.c, d);
            }
        }
        break;
    case Sampling::Ycbcr420:
        for (uint32_t x = 0; x < width; x += 2) {
            const Comps a00 = toComps(pf, row0[x]);
            const Comps a01 = toComps(pf, row0[x + 1]);
            const Comps a10 = toComps(pf, row1[x]);
            const Comps a11 = toComps(pf, row1[x + 1]);
            const Comps c = average(average(a00, a01), average(a10, a11));
            w.put(a00.a, d);
            w.put(a01.a, d);
            w.put(a10.a, d);
            w.put(a11.a, d);
            w.put(c.b, d);
            w.put(c.c, d);
        }
        break;
    }
}

} // namespace

PixelPatternGenerator::PixelPatternGenerator(const PixelFormat& format, uint32_t width, uint32_t height,
                                             bool interlaced)
    : format_(format), width_(width), height_(height), interlaced_(interlaced) {
    rowsPerUnit_ = format.sampling == Sampling::Ycbcr420 ? 2 : 1;
    // Spalten je Pixelgruppe in einer Einheitszeile (4:2:0: eine Gruppe deckt rowsPerUnit_ Zeilen ab)
    align_ = format.pgroupPixels() / rowsPerUnit_;
    unitRows_ = (interlaced ? height / 2 : height) / rowsPerUnit_;
    lineBytes_ = static_cast<size_t>(width / format.pgroupPixels()) * format.pgroupBytes();
    // Eine Einheit = rowsPerUnit_ Pufferzeilen (bei 4:2:0 zwei Pufferzeilen je Blockzeile aus zwei
    // Bildzeilen)
    unitBytes_ = lineBytes_ * rowsPerUnit_;
    // Bänder wie TestPatternGenerator (Höhen in Bildzeilen); bei 4:2:0 auf Blockzeilen gerundet
    barsEnd_ = height * 2 / 3;
    rampEnd_ = height * 5 / 6;
    if (rowsPerUnit_ == 2) {
        barsEnd_ &= ~1u;
        rampEnd_ &= ~1u;
    }
    // Quadrat: Breite ein Vielfaches der Pixelgruppe, Höhe ein Vielfaches der Einheit
    const uint32_t step = std::lcm(align_, rowsPerUnit_);
    box_ = std::max<uint32_t>(step, (std::max<uint32_t>(4, height / 6) / step) * step);
    boxY_ = barsEnd_ + (rampEnd_ - barsEnd_ - std::min(box_, rampEnd_ - barsEnd_)) / 2;
    if (rowsPerUnit_ == 2) {
        boxY_ &= ~1u;
    }

    PreviewImage image;
    renderTestPattern(image, width, height, 0.5, 0, false);
    PreviewImage flash;
    renderTestPattern(flash, width, height, 0.0, 0, false);
    auto rowOf = [&](const PreviewImage& img, uint32_t y) {
        return reinterpret_cast<const Rgb8*>(img.rgb.data() + static_cast<size_t>(y) * width * 3);
    };
    auto unit = [&](const PreviewImage& img, uint32_t y) {
        std::vector<uint8_t> out(unitBytes_);
        packUnit(format, rowOf(img, y), rowOf(img, std::min(y + rowsPerUnit_ - 1, height - 1)), width,
                 out.data());
        return out;
    };
    barsUnit_ = unit(image, 0);
    rampUnit_ = unit(image, barsEnd_);
    bottomUnit_ = unit(image, height - rowsPerUnit_);
    flashUnit_ = unit(flash, height - rowsPerUnit_);

    // Pixelgruppe in der Farbe des Quadrats: eine Zeile (bei 4:2:0 zwei) aus lauter gleichen Bildpunkten
    const Rgb8 color{235, 64, 32};
    std::vector<Rgb8> row(align_, color);
    std::vector<uint8_t> group(format.pgroupBytes());
    packUnit(format, row.data(), row.data(), align_, group.data());
    boxGroup_ = std::move(group);
}

uint32_t PixelPatternGenerator::unitsPerFrame() const {
    return unitRows_;
}

void PixelPatternGenerator::render(uint8_t* dst, double t, bool secondField) const {
    const bool flash = std::fmod(t, 1.0) < 0.08;
    const double phase = std::fmod(t * 0.25, 1.0);
    const uint32_t boxX = (static_cast<uint32_t>(phase * (width_ - box_)) / align_) * align_;
    const size_t groupBytes = format_.pgroupBytes();
    for (uint32_t u = 0; u < unitRows_; ++u) {
        // Bildzeile der Einheit: bei Zeilensprung jede zweite Zeile des Vollbilds
        const uint32_t y = interlaced_ ? 2 * u + (secondField ? 1 : 0) : u * rowsPerUnit_;
        uint8_t* row = dst + u * unitBytes_;
        const std::vector<uint8_t>* src;
        if (y < barsEnd_) {
            src = &barsUnit_;
        } else if (y < rampEnd_) {
            src = &rampUnit_;
        } else {
            src = flash ? &flashUnit_ : &bottomUnit_;
        }
        std::memcpy(row, src->data(), unitBytes_);
        if (y >= boxY_ && y < boxY_ + box_) {
            // Quadrat: Pixelgruppen der Einheitszeile überschreiben
            for (uint32_t x = boxX; x < boxX + box_ && x < width_; x += align_) {
                std::memcpy(row + static_cast<size_t>(x / align_) * groupBytes, boxGroup_.data(), groupBytes);
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
