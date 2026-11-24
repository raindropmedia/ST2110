#pragma once

#include "core/TimeSeries.h"

#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace st2110 {

// Nutzdaten-Header nach SMPTE ST 2022-6:2012, Abschnitt 6.4 (High Bit Rate Media Payload Header):
//   Wort 0: Ext(4) F(1) VSID(3) FRCount(8) R(2) S(2) FEC(3) CF(4) RESERVE(5)
//   Wort 1: MAP(4) FRAME(8) FRATE(8) SAMPLE(4) FMT-RESERVE(8)
//   dann Video-Zeitstempel (32 Bit, nur bei CF > 0) und Erweiterung (Ext × 4 Byte).
struct HbrmtHeader {
    uint8_t ext = 0;
    bool f = false;
    uint8_t vsid = 0;
    uint8_t frCount = 0;
    uint8_t r = 0;
    uint8_t s = 0;
    uint8_t fec = 0;
    uint8_t cf = 0;
    uint8_t reserve = 0;
    uint8_t map = 0;
    uint8_t frame = 0;
    uint8_t frate = 0;
    uint8_t sample = 0;
    uint8_t fmtReserve = 0;
    uint32_t videoTimestamp = 0; // gültig, wenn cf > 0
    size_t length = 0;           // Länge des Headers in Byte (8, 12 oder mehr mit Erweiterung)
};

// Liest den Header; false, wenn die Daten dafür zu kurz sind.
bool parseHbrmtHeader(const uint8_t* data, size_t size, HbrmtHeader& out);

// Klartext zu den Codes der Norm (leer bzw. "reserviert", wenn der Code nicht definiert ist).
std::string hbrmtFrameName(uint8_t frame);
std::string hbrmtRateName(uint8_t frate);
std::string hbrmtSampleName(uint8_t sample);
std::string hbrmtClockName(uint8_t cf);
// Taktfrequenz des Video-Zeitstempels in Hz (0 = kein Zeitstempel/reserviert).
double hbrmtClockHz(uint8_t cf);
// Bilder je Sekunde laut FRATE (0 = unbekannt).
double hbrmtFrameRateHz(uint8_t frate);
// Erwartete Datagramme je Bild nach ST 2022-6 Abschnitt 6.5 (DPF = Oktette je Bild / 1376 + 1) aus den
// Rastermaßen von ST 274 (1080), ST 296 (720) und ST 259/ITU-R BT.656 (SD); 0 bei anderen Formaten.
uint32_t hbrmtExpectedDatagrams(uint8_t frame, uint8_t frate, uint8_t sample);

// Wertet einen ST-2022-6-Strom Paket für Paket aus (RTP-Passthrough von MTL: die Anwendung bekommt die
// RTP-Pakete). ST 2022-6 hat kein Netzwerk-Zeitmodell; geprüft werden RTP- und Nutzdaten-Header, Folge der
// Sequenznummern, Bildaufbau (Marker, FRCount, Datagramme je Bild) und der Zeitstempelschritt.
class HbrmtAnalyzer {
  public:
    enum class Level { Ok, Warn, Fail };

    HbrmtAnalyzer();

    // Ein RTP-Paket (ab dem RTP-Header, wie von st20_rx_get_mbuf geliefert). Liefert eine Meldung, wenn sich
    // das Urteil geändert hat oder eine einmalige Auffälligkeit auftrat (leer sonst).
    struct Note {
        Level level = Level::Ok;
        std::string text;
    };
    std::optional<Note> addPacket(const uint8_t* rtp, size_t size, double nowSec);
    void reset();
    Level verdict(double nowSec) const;
    bool hasPackets() const { return packets_ > 0; }
    uint64_t packets() const { return packets_; }
    uint64_t frames() const { return frames_; }

    nlohmann::json toJson(double nowSec, double windowSec, size_t maxPoints) const;
    nlohmann::json system10Checks() const;

  private:
    uint64_t packets_ = 0;
    uint64_t bytes_ = 0;
    uint64_t frames_ = 0;
    uint64_t lost_ = 0;
    uint64_t reordered_ = 0;
    uint64_t badRtp_ = 0;    // Version, P, X, CC
    uint64_t badPt_ = 0;     // Payload-Typ nicht 98/99
    uint64_t badSize_ = 0;   // Nutzlast ungleich 1376 Byte
    uint64_t badHeader_ = 0; // F = 0, Header zu kurz
    uint64_t frCountErrors_ = 0;
    uint64_t formatChanges_ = 0;
    uint64_t stepErrors_ = 0;
    uint64_t dpfErrors_ = 0;

    bool haveSeq_ = false;
    uint16_t nextSeq_ = 0;
    bool haveFrame_ = false;
    uint8_t frameFrCount_ = 0;
    uint32_t frameFirstTs_ = 0;
    uint32_t frameDatagrams_ = 0;
    bool haveLastFrameTs_ = false;
    uint32_t lastFrameTs_ = 0;
    bool havePrevFrCount_ = false;
    uint8_t prevFrCount_ = 0;
    bool afterMarker_ = true;

    HbrmtHeader header_;
    bool haveHeader_ = false;
    uint8_t ptSeen_ = 0;
    uint32_t ssrc_ = 0;
    bool haveSsrc_ = false;
    uint64_t ssrcChanges_ = 0;

    uint32_t dpfLast_ = 0;
    uint32_t dpfMin_ = 0xFFFFFFFF;
    uint32_t dpfMax_ = 0;
    uint32_t dpfExpected_ = 0;
    int64_t lastStep_ = 0;
    int64_t minStep_ = 0;
    int64_t maxStep_ = 0;
    bool haveStep_ = false;

    // Video-Zeitstempel minus RTP-Zeitstempel (nur CF = 27 MHz, beide im selben Takt)
    bool haveVts_ = false;
    int32_t vtsOffsetMin_ = 0;
    int32_t vtsOffsetMax_ = 0;

    double secondStart_ = -1;
    uint64_t secondPackets_ = 0;
    uint64_t secondBytes_ = 0;
    double lastMbit_ = 0;
    double lastPps_ = 0;

    Level lastNotified_ = Level::Ok;
    double lastFailSec_ = -1e9;
    double lastWarnSec_ = -1e9;
    std::string lastCause_;
    TimeSeries mbitSeries_;
    TimeSeries dpfSeries_;
};

} // namespace st2110
