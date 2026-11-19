#pragma once

#include "core/Histogram.h"
#include "core/Path.h"
#include "core/TimingStats.h"
#include "core/VideoTimingModel.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace st2110 {

// Ankunft aller Pakete eines Bildes: Grundlage der Paketprofil-Ansicht (Ankunft gegen Sollzeit,
// Cinst und VRX je Paket über dem Bild).
struct PacketProfile {
    uint32_t rtpTimestamp = 0;
    std::vector<int32_t> index;   // Paketindex im Bild
    std::vector<float> arrivalUs; // Ankunft relativ zum Bildbeginn (Epoche) in µs
    std::vector<int16_t> cinst;   // Cinst nach dem Paket
    std::vector<int16_t> vrx;     // VRX nach dem Paket
    FrameTiming timing;           // Kennwerte des Bildes

    bool empty() const { return index.empty(); }
    void clear();
};

// Eigene Auswertung je Paket nach ST 2110-21 (Rechenweg wie der Timing-Parser von MTL, aber mit
// ganzzahligen Zeitabständen): Histogramme für IPT, Cinst und VRX, Kennwerte je Bild und Paketprofile.
//
// Zeitstempel: Ankunft in ns (TAI/PTP). Nicht threadsicher; der Aufrufer serialisiert.
class VideoPacketAnalyzer {
  public:
    // Größte Paketzahl je Bild, die ein Profil aufnimmt (2160p 10 Bit ≈ 17 300 Pakete).
    static constexpr size_t kMaxProfilePackets = 32768;

    VideoPacketAnalyzer(uint32_t height, bool interlaced, FrameRate rate);

    // Paketzahl je Bild von außen vorgeben (z. B. aus MTL); sonst lernt die Analyse sie aus zwei
    // gleich langen Bildern.
    void setPacketsPerFrame(int packets);
    const std::optional<VideoTimingParams>& params() const { return params_; }

    // Verarbeitet ein Paket. Beginnt damit ein neues Bild, liefert die Funktion die Kennwerte des
    // vorherigen Bildes dieses Pfades.
    std::optional<FrameTiming> onPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs,
                                        int packetIndex);
    // Schließt das laufende Bild eines Pfades ab (z. B. am Messende).
    std::optional<FrameTiming> flush(Path path);

    struct Histograms {
        Histogram iptNs;
        Histogram cinst;
        Histogram vrx;
        void clear();
        void merge(const Histograms& other);
    };
    // Seit Beginn bzw. seit resetTotals(), und seit dem letzten takeInterval().
    const Histograms& totals(Path path) const { return paths_[pathIndex(path)].total; }
    Histograms takeInterval(Path path);
    const PacketProfile& lastProfile(Path path) const { return paths_[pathIndex(path)].last; }
    const PacketProfile& worstProfile(Path path) const { return paths_[pathIndex(path)].worst; }
    void resetTotals();

  private:
    struct PathState {
        bool inFrame = false;
        bool frameHasParams = false;
        uint32_t rtp = 0;
        bool hasPrevRtp = false;
        uint32_t prevRtp = 0;
        long double epochStartNs = 0;
        long double firstPacketNs = 0;
        uint64_t prevArrivalNs = 0;
        bool hasPrevArrival = false;
        int maxIndex = -1;
        FrameTiming frame;
        int64_t cinstSum = 0;
        int64_t vrxSum = 0;
        int64_t iptSum = 0;
        uint32_t iptCount = 0;
        PacketProfile current;
        PacketProfile last;
        PacketProfile worst;
        Histograms total;
        Histograms interval;
        int lastFrameMaxIndex = -1;
    };

    void ensureHistograms(PathState& state);
    void startFrame(PathState& state, uint32_t rtpTimestamp, uint64_t arrivalNs, int packetIndex);
    std::optional<FrameTiming> finishFrame(PathState& state);
    void learnPacketCount(int maxIndex, int previousMaxIndex);

    uint32_t height_;
    bool interlaced_;
    FrameRate rate_;
    double frameTimeNs_;
    double rtpTicksPerFrame_;
    std::optional<VideoTimingParams> params_;
    std::array<PathState, kPathCount> paths_;
};

} // namespace st2110
