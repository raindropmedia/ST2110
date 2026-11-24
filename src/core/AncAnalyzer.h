#pragma once

#include "core/FrameRate.h"
#include "core/Histogram.h"
#include "core/Sdp.h"
#include "core/TimeSeries.h"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace st2110 {

// Ein ANC-Datenpaket (RFC 8331 / ST 291-1) aus den Angaben von MTL (st40_meta). DID und SDID liegen ohne
// Paritätsbits vor (8 Bit); MTL hat Parität und Prüfsumme der Nutzdaten schon geprüft.
struct AncPacket {
    uint16_t line = 0;       // Zeilennummer im SDI-Raster
    uint16_t horiOffset = 0; // Position in der Zeile
    uint8_t did = 0;
    uint8_t sdid = 0;
    uint16_t udwSize = 0; // Nutzdatenwörter
    bool luma = false;    // C-Bit: Luma- statt Chroma-Kanal
    uint8_t streamNum = 0;
};

// Ein ANC-Bild (alle RTP-Pakete mit demselben RTP-Zeitstempel), wie MTL es auf Frame-Ebene liefert.
struct AncFrameReport {
    uint32_t rtpTimestamp = 0;  // 90 kHz
    uint64_t firstPacketNs = 0; // TAI, Ankunft des ersten Pakets (Hardware-Zeit, falls verfügbar)
    uint32_t packets = 0;       // RTP-Pakete des Bildes
    uint32_t seqLost = 0;       // fehlende Sequenznummern im Bild
    bool seqDiscont = false;
    bool marker = false; // RTP-Marker beobachtet (letztes Paket des Bildes)
    bool interlaced = false;
    bool secondField = false;
    bool corrupted = false; // MTL-Status "corrupted"
    std::vector<AncPacket> anc;
};

// Name der Datenart nach SMPTE-Registrierung (RP 291-2 / ST 291-1); nur sicher bekannte Kennungen, sonst "".
const char* ancDataName(uint8_t did, uint8_t sdid);

// Wertet ST-2110-40-Bilder aus: Zählung je DID/SDID, Zeilen, RTP-Zeitstempelschritt, Zeitbezug zur PTP-Zeit
// (Ankunft des ersten Pakets gegen die Sollzeit aus dem RTP-Zeitstempel), Verluste, Marker. ST 2110-40 kennt
// kein Netzwerk-Zeitmodell wie ST 2110-21; das Urteil beruht deshalb auf beobachteten Fehlern, nicht auf
// narrow/wide.
class AncAnalyzer {
  public:
    enum class Level { Ok, Warn, Fail };
    struct FrameResult {
        Level level = Level::Ok;
        std::string cause; // leer bei Ok
    };

    explicit AncAnalyzer(const VideoFormat& format);

    FrameResult add(const AncFrameReport& frame, double nowSec);
    // Gesamtwerte und Verläufe löschen (Zurücksetzen).
    void reset();
    // Urteil über die letzten 10 s (Fail vor Warn vor Ok); Ok, solange noch kein Bild kam.
    Level verdict(double nowSec) const;
    bool hasFrames() const { return frames_ > 0; }
    uint64_t frames() const { return frames_; }

    nlohmann::json toJson(double nowSec, double windowSec, size_t maxPoints) const;
    // Zeitbezug-Prüfungen für die Systemprüfung ST 2110-10 (Format wie System10).
    nlohmann::json system10Checks() const;

  private:
    struct DataStats {
        uint64_t count = 0;
        uint64_t udwWords = 0;
        uint64_t framesWith = 0;
        uint32_t maxPerFrame = 0;
        uint16_t minLine = 0xFFFF;
        uint16_t maxLine = 0;
        uint16_t lastLine = 0;
        uint16_t lastHori = 0;
        uint16_t lastUdw = 0;
        bool luma = false;
        double lastSeenSec = 0;
    };

    FrameRate rate_;
    bool interlaced_;
    double periodNs_;
    int expectedStepMin_;
    int expectedStepMax_;

    uint64_t frames_ = 0;
    uint64_t corrupted_ = 0;
    uint64_t empty_ = 0;
    uint64_t noMarker_ = 0;
    uint64_t rtpPackets_ = 0;
    uint64_t ancPackets_ = 0;
    uint64_t udwWords_ = 0;
    uint64_t seqLost_ = 0;
    uint64_t discont_ = 0;
    uint64_t stepErrors_ = 0;
    uint64_t latencyWarnings_ = 0;
    uint32_t perFrameMin_ = 0xFFFFFFFF;
    uint32_t perFrameMax_ = 0;
    uint64_t perFrameSum_ = 0;

    bool haveLastTs_ = false;
    uint32_t lastTs_ = 0;
    int32_t lastStep_ = 0;
    int32_t minStep_ = 0;
    int32_t maxStep_ = 0;
    bool haveStep_ = false;

    double latencyMinUs_ = 0;
    double latencyMaxUs_ = 0;
    double latencySumUs_ = 0;
    uint64_t latencyCount_ = 0;
    int64_t lastLatencyNs_ = 0;
    Histogram latencyHist_;

    double lastFailSec_ = -1e9;
    double lastWarnSec_ = -1e9;
    std::string lastCause_;
    std::map<std::pair<uint8_t, uint8_t>, DataStats> data_;
    AncFrameReport last_;
    TimeSeries perFrameSeries_;
    TimeSeries latencySeries_;
};

} // namespace st2110
