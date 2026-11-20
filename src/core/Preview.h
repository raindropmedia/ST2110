#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace st2110 {

// Verkleinertes Vorschaubild (RGB, 8 Bit je Farbe, Zeilen ohne Füllbytes).
struct PreviewImage {
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgb;
    uint64_t sequence = 0;
    double timeSec = 0; // Zeitpunkt der Aufnahme (Monitorzeit)
    std::string jpeg;   // komprimiert (für die Oberfläche); rgb darf danach leer sein
};

// Gewünschte Vorschau: größte Breite (Bilder werden durch Auslassen verkleinert, nie vergrößert),
// Bildrate und JPEG-Qualität. Die zuletzt angeforderten Einstellungen gelten.
struct PreviewSettings {
    uint32_t maxWidth = 480;
    double fps = 5;
    int quality = 80;
};

// Komprimiert image.rgb als JPEG (Baseline, 4:2:0) nach image.jpeg; false bei Fehlern.
bool encodeJpeg(PreviewImage& image, int quality);

// Wandelt ein Bild bzw. Halbbild im Transportformat von ST 2110-20 (YCbCr 4:2:2, 10 Bit, RFC 4175:
// je 2 Pixel 5 Byte Cb Y0 Cr Y1, Big Endian) nach RGB und verkleinert es durch Auslassen auf höchstens
// maxWidth Pixel Breite. field: bei Halbbildern (lines = halbe Höhe) wird jede Zeile doppelt gezählt,
// damit das Seitenverhältnis stimmt (volle Auflösung: Zeilenverdopplung). Farbumrechnung BT.709,
// begrenzter Bereich, über Tabellen.
// Liefert false bei ungültigen Angaben.
bool convertRfc4175ToRgb(const uint8_t* frame, size_t frameBytes, uint32_t width, uint32_t lines, bool field,
                         uint32_t maxWidth, PreviewImage& out);

// Testbild für die Demo: Farbbalken (75 %), Grautreppe, bewegtes Quadrat; variant verschiebt den
// Farbton (unterscheidbare Kameras), glitch zeichnet schwarze Störzeilen (Paketverlust).
void renderTestPattern(PreviewImage& out, uint32_t width, uint32_t height, double timeSec, int variant,
                       bool glitch);

// Vorschaubild eines Flows. Threadsicher; eigener Mutex, unabhängig vom Monitor des Flows.
// Bilder entstehen nur, solange die Oberfläche sie anfordert (request()), und höchstens alle intervalSec.
class VideoPreview {
  public:
    static constexpr double kKeepAliveSec = 3.0;
    static constexpr double kMaxFps = 50.0;

    void request(double nowSec, const PreviewSettings& settings = {});
    PreviewSettings settings() const;
    // true, wenn ein neues Bild gebraucht wird (angefordert und Intervall 1/fps abgelaufen).
    bool due(double nowSec) const;
    void put(PreviewImage image, double nowSec);
    // Letztes Bild (leer, wenn keines vorliegt).
    PreviewImage latest() const;
    // Wartet höchstens timeoutSec auf ein Bild mit höherer Nummer als afterSequence und liefert dann
    // das letzte Bild (Long Polling; den Takt gibt die Bildrate des Messgeräts vor).
    PreviewImage waitNewer(uint64_t afterSequence, double timeoutSec) const;

  private:
    mutable std::mutex mutex_;
    mutable std::condition_variable newImage_;
    double requestedAt_ = -1e9;
    double lastPut_ = -1e9;
    uint64_t sequence_ = 0;
    PreviewSettings settings_;
    PreviewImage image_;
};

// Tonvorschau eines Flows: Ringpuffer der letzten Sekunden (alle Kanäle, float) mit fortlaufendem
// Abtastzähler, dazu Spitzenpegel je Kanal. Threadsicher.
class AudioPreview {
  public:
    static constexpr double kKeepAliveSec = 3.0;
    static constexpr size_t kMaxChannels = 64;

    void configure(uint32_t sampleRate, uint16_t channels);
    void request(double nowSec);
    bool wanted(double nowSec) const;

    // PCM im Transportformat (Big Endian, verschachtelt, 16 oder 24 Bit).
    void pushPcm(const uint8_t* data, size_t bytes, uint8_t bitDepth);
    // Bereits normierte Abtastwerte (-1 … 1), verschachtelt (Demo).
    void pushFloat(const float* samples, size_t frames);

    struct Chunk {
        uint64_t start = 0; // Index des ersten Abtastwerts
        uint32_t frames = 0;
        uint32_t sampleRate = 0;
        uint16_t channels = 0;        // Kanäle im Strom
        std::vector<int16_t> stereo;  // gewähltes Kanalpaar, verschachtelt, 16 Bit
        std::vector<float> peaksDbfs; // Spitzenpegel je Kanal seit der letzten Abfrage
    };
    // Abtastwerte ab from (bzw. die letzten leadSec, wenn from veraltet oder 0 ist), höchstens maxFrames.
    // pair: 0 = Kanäle 1/2, 1 = 3/4 …; bei einem Kanal wird er auf beide Seiten gelegt.
    Chunk read(uint64_t from, unsigned pair, uint32_t maxFrames, double leadSec = 0.25);

  private:
    mutable std::mutex mutex_;
    uint32_t sampleRate_ = 48000;
    uint16_t channels_ = 2;
    std::vector<float> ring_; // capacity_ Frames x channels_
    size_t capacity_ = 0;
    uint64_t written_ = 0; // Frames insgesamt
    std::array<float, kMaxChannels> peak_{};
    double requestedAt_ = -1e9;
};

// Testton für die Demo: Kanal 1 1 kHz mit Unterbrechung (250 ms alle 3 s, EBU-Kennung), Kanal 2 2 kHz,
// weitere Kanäle 500 Hz · Kanalnummer; Pegel −18 dBFS. Schreibt frames · channels Werte ab sampleIndex.
void generateTestTone(std::vector<float>& out, uint64_t sampleIndex, size_t frames, uint16_t channels,
                      uint32_t sampleRate);

} // namespace st2110
