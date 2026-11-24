#pragma once

#include "core/PixelFormat.h"

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

// Testbild für beliebige Pixelformate nach ST 2110-20: derselbe Inhalt wie TestPatternGenerator, aber in der
// Pixelgruppen-Aufteilung von Abtastung und Tiefe (RFC 4175, MSB zuerst):
//   4:2:2  Cb Y0 Cr Y1             (2 Bildpunkte)
//   4:4:4  Cb Y Cr / RGB: R G B    (1 Bildpunkt)
//   4:2:0  Y00 Y01 Y10 Y11 Cb Cr   (2×2 Bildpunkte)
// YCbCr im begrenzten Bereich (Y 16…235, C 16…240 bei 8 Bit, bei höherer Tiefe um 2^(Tiefe−8) verschoben),
// BT.709; RGB im vollen Bereich. Das Bild liegt als lückenlose Folge von Pixelgruppen im Puffer, so wie MTL
// es erwartet (bei 4:2:0 gehören je zwei Zeilen des Puffers zu einer Blockzeile aus zwei Bildzeilen). 4:2:0
// mit Zeilensprung gibt es nicht. Bei 4:2:2 mit 10 Bit ist die Ausgabe bitgleich zu TestPatternGenerator.
class PixelPatternGenerator {
  public:
    // width muss ein Vielfaches der Pixelgruppe sein, height bei 4:2:0 gerade.
    PixelPatternGenerator(const PixelFormat& format, uint32_t width, uint32_t height, bool interlaced);

    // Bytes eines Bildes bzw. Halbbildes (Größe des Puffers).
    size_t frameBytes() const { return unitBytes_ * unitsPerFrame(); }
    // Bytes einer Pufferzeile (bei 4:2:0 die halbe Blockzeile).
    size_t lineBytes() const { return lineBytes_; }
    uint32_t linesPerFrame() const { return interlaced_ ? height_ / 2 : height_; }

    // Schreibt ein Bild (Halbbild) lückenlos nach dst (frameBytes() Byte).
    void render(uint8_t* dst, double timeSec, bool secondField = false) const;

    // Eine einzelne Zeile (Einheit) als Bytefolge, zum Prüfen: Bildzeile y (bei 4:2:0 Blockzeile y/2).
    const PixelFormat& format() const { return format_; }

  private:
    uint32_t unitsPerFrame() const;
    size_t unitBytes_ = 0;
    size_t lineBytes_ = 0;
    PixelFormat format_;
    uint32_t width_;
    uint32_t height_;
    bool interlaced_;
    uint32_t unitRows_;    // Einheiten je Bild (Bildzeilen bzw. Blockzeilen)
    uint32_t rowsPerUnit_; // 1, bei 4:2:0 2
    uint32_t barsEnd_;
    uint32_t rampEnd_;
    uint32_t box_;
    uint32_t boxY_;
    uint32_t align_; // Spalten je Pixelgruppe in einer Einheitszeile: Raster für Lage und Breite des Quadrats
    std::vector<uint8_t> barsUnit_;
    std::vector<uint8_t> rampUnit_;
    std::vector<uint8_t> bottomUnit_;
    std::vector<uint8_t> flashUnit_;
    std::vector<uint8_t> boxGroup_; // eine Pixelgruppe in der Farbe des Quadrats
};

// Testton (wie generateTestTone: 1 kHz links mit EBU-Kennung, 2 kHz rechts, −18 dBFS) als PCM Big Endian
// mit 16 oder 24 Bit, verschachtelt, für frames Abtastwerte je Kanal ab sampleIndex.
void renderTestTonePcm(uint8_t* dst, uint64_t sampleIndex, size_t frames, uint16_t channels,
                       uint32_t sampleRate, uint8_t bitDepth);

} // namespace st2110
