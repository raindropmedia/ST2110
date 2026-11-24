#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st2110 {

// Abtastung und Bittiefe eines Videoflows nach ST 2110-20 (RFC 4175). Das sind die 16 Kombinationen, die MTL
// als st20_fmt kennt: YCbCr 4:2:2, 4:2:0, 4:4:4 und RGB mit je 8, 10, 12 und 16 Bit. Nicht dabei sind
// 16-Bit-Gleitkomma (16f), XYZ, CLYCbCr, ICtCp, Key und die Farbtiefe 14.
enum class Sampling { Ycbcr422, Ycbcr420, Ycbcr444, Rgb };

struct PixelFormat {
    Sampling sampling = Sampling::Ycbcr422;
    int depth = 10;

    bool operator==(const PixelFormat& o) const { return sampling == o.sampling && depth == o.depth; }
    bool operator!=(const PixelFormat& o) const { return !(*this == o); }
    bool isDefault() const { return sampling == Sampling::Ycbcr422 && depth == 10; }

    // Komponenten und Bildpunkte eines kleinsten Blocks: 4:2:2 Cb Y0 Cr Y1 (4 Werte, 2 Bildpunkte),
    // 4:4:4/RGB ein Bildpunkt (3 Werte), 4:2:0 2×2 Bildpunkte (Y00 Y01 Y10 Y11 Cb Cr, 6 Werte).
    uint32_t samplesPerBlock() const;
    uint32_t pixelsPerBlock() const;
    // Pixelgruppe (RFC 4175): kleinste Zahl ganzer Blöcke mit ganzzahliger Bytezahl.
    uint32_t pgroupBlocks() const;
    uint32_t pgroupBytes() const;
    uint32_t pgroupPixels() const;
    double bytesPerPixel() const { return static_cast<double>(pgroupBytes()) / pgroupPixels(); }

    std::string sdpSampling() const; // "YCbCr-4:2:2", "RGB" …
    std::string name() const;        // "YCbCr-4:2:2 10 Bit"
};

// "YCbCr-4:2:2", "422", "RGB" … (ohne Beachtung der Groß-/Kleinschreibung)
std::optional<Sampling> parseSampling(const std::string& text);
const char* samplingName(Sampling sampling); // "YCbCr-4:2:2" …
bool validDepth(int depth);                  // 8, 10, 12, 16

// Alle 16 Formate in fester Reihenfolge (nach Abtastung, dann Tiefe).
const std::vector<PixelFormat>& allPixelFormats();

// Packmodus nach ST 2110-20: Block (BPM), allgemein (GPM), allgemein mit einer Zeile je Paket (GPM_SL).
enum class Packing { Bpm, Gpm, GpmSl };
const char* packingName(Packing packing); // "BPM", "GPM", "GPM_SL"
std::optional<Packing> parsePacking(const std::string& text);

} // namespace st2110
