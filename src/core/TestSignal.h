#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace st2110 {

// Ein Bildpunkt in YCbCr, 10 Bit, begrenzter Bereich (Y 64…940, Cb/Cr 64…960).
struct Ycbcr10 {
    uint16_t y = 64;
    uint16_t cb = 512;
    uint16_t cr = 512;
};

// RGB (8 Bit, voller Bereich) nach YCbCr 10 Bit, BT.709, begrenzter Bereich.
Ycbcr10 rgbToYcbcr709(uint8_t r, uint8_t g, uint8_t b);

// Testbild für den Testsender im Transportformat von ST 2110-20 (YCbCr 4:2:2, 10 Bit, RFC 4175, je 2
// Bildpunkte 5 Byte Cb Y0 Cr Y1, Big Endian). Gleicher Inhalt wie renderTestPattern() der Vorschau:
// 75-%-Farbbalken, Grautreppe, ein waagerecht laufendes Quadrat und ein Blitz am Anfang jeder Sekunde
// (rechts unten). Die Zeilen der Bänder werden einmal berechnet; je Bild werden nur Zeilen kopiert und
// das Quadrat gezeichnet, damit ein Kern 1080p50 sicher schafft.
class TestPatternGenerator {
  public:
    // width muss gerade sein; height ist die Bildhöhe (bei Zeilensprung die des Vollbilds).
    TestPatternGenerator(uint32_t width, uint32_t height, bool interlaced);

    // Bytes einer Zeile ohne Füllbytes.
    size_t lineBytes() const { return lineBytes_; }
    // Zeilen je übertragenem Bild bzw. Halbbild.
    uint32_t linesPerFrame() const { return interlaced_ ? height_ / 2 : height_; }

    // Schreibt ein Bild (bzw. Halbbild: secondField wählt die ungeraden Zeilen) nach dst. linesize ist
    // der Abstand zweier Zeilen in dst (>= lineBytes()). timeSec bestimmt Lage des Quadrats und Blitz.
    void render(uint8_t* dst, size_t linesize, double timeSec, bool secondField = false) const;

  private:
    enum class Band { Bars, Ramp, Bottom };
    Band bandOf(uint32_t line) const;

    uint32_t width_;
    uint32_t height_;
    bool interlaced_;
    size_t lineBytes_;
    uint32_t barsEnd_;
    uint32_t rampEnd_;
    uint32_t box_;
    uint32_t boxY_;
    std::vector<uint8_t> barsLine_;
    std::vector<uint8_t> rampLine_;
    std::vector<uint8_t> bottomLine_;
    std::vector<uint8_t> flashLine_;
    std::vector<uint8_t> boxPair_; // ein Paar Bildpunkte in der Farbe des Quadrats
};

// Testton (wie generateTestTone: 1 kHz links mit EBU-Kennung, 2 kHz rechts, −18 dBFS) als PCM Big Endian
// mit 16 oder 24 Bit, verschachtelt, für frames Abtastwerte je Kanal ab sampleIndex.
void renderTestTonePcm(uint8_t* dst, uint64_t sampleIndex, size_t frames, uint16_t channels,
                       uint32_t sampleRate, uint8_t bitDepth);

} // namespace st2110
