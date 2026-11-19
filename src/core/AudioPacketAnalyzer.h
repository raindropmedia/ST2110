#pragma once

#include "core/AudioFormat.h"
#include "core/Histogram.h"
#include "core/Path.h"
#include "core/TimingStats.h"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>

namespace st2110 {

// Kennwerte eines Auswertezeitraums (MTL: 200 ms) für einen Tonpfad, entspricht st30_rx_tp_meta.
struct AudioTiming {
    int32_t dpvrMinUs = 0; // Delta Packet vs RTP: Ankunft minus RTP-Sollzeit
    int32_t dpvrMaxUs = 0;
    float dpvrAvgUs = 0;
    int32_t iptMinNs = 0;
    int32_t iptMaxNs = 0;
    float iptAvgNs = 0;
    int32_t tsdfUs = 0; // Timestamped Delay Factor (Spannweite der DPVR im Zeitraum)
    Compliance compliance = Compliance::Failed;
    std::string failedCause;
    uint32_t packets = 0;
};

// Grenzwerte wie MTL v26.09 (ra_tp_init): DPVR narrow 3, wide 19 Paketzeiten; TSDF narrow 1,
// wide 17 Paketzeiten (jeweils in µs).
struct AudioPassCriteria {
    int32_t dpvrMaxNarrowUs = 0;
    int32_t dpvrMaxWideUs = 0;
    int32_t tsdfMaxNarrowUs = 0;
    int32_t tsdfMaxWideUs = 0;
};

AudioPassCriteria computeAudioPassCriteria(PacketTime ptime);
// Urteil wie MTL (ra_tp_slot_compliant).
Compliance evaluateAudioPeriod(const AudioTiming& timing, const AudioPassCriteria& pass, std::string& cause);

// Sammelt Zeiträume eines Tonpfades über ein Intervall.
class AudioStats {
  public:
    void add(const AudioTiming& timing);
    void merge(const AudioStats& other);
    void reset() { *this = AudioStats{}; }
    uint64_t periods() const { return periods_; }
    uint64_t packets() const { return packets_; }
    uint64_t count(Compliance c) const { return counts_[static_cast<size_t>(c)]; }
    Compliance worst() const;
    const Range& dpvrUs() const { return dpvr_; }
    const Range& iptNs() const { return ipt_; }
    const Range& tsdfUs() const { return tsdf_; }
    const std::map<std::string, uint64_t>& failedCauses() const { return causes_; }

  private:
    uint64_t periods_ = 0;
    uint64_t packets_ = 0;
    std::array<uint64_t, 3> counts_{};
    Range dpvr_;
    Range ipt_;
    Range tsdf_;
    std::map<std::string, uint64_t> causes_;
};

// Eigene Auswertung je Tonpaket (Rechenweg wie ra_tp_on_packet in MTL, RTP-Differenz überlaufsicher):
// Histogramme für DPVR und IPT, Zeiträume von 200 ms Paketzeit mit TSDF und Urteil.
class AudioPacketAnalyzer {
  public:
    static constexpr double kPeriodNs = 200e6;

    explicit AudioPacketAnalyzer(const AudioFormat& format);

    const AudioFormat& format() const { return format_; }
    const AudioPassCriteria& pass() const { return pass_; }

    // Liefert einen abgeschlossenen Zeitraum, sobald 200 ms seit dessen Beginn vergangen sind.
    std::optional<AudioTiming> onPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs);
    std::optional<AudioTiming> flush(Path path);

    struct Histograms {
        Histogram dpvrUs;
        Histogram iptNs;
        void clear();
        void merge(const Histograms& other);
    };
    const Histograms& totals(Path path) const { return paths_[pathIndex(path)].total; }
    Histograms takeInterval(Path path);
    void resetTotals();

    // DPVR eines Pakets in µs (für Tests und Demo).
    double dpvrUs(uint32_t rtpTimestamp, uint64_t arrivalNs) const;

  private:
    struct PathState {
        bool inPeriod = false;
        uint64_t periodStartNs = 0;
        uint64_t prevArrivalNs = 0;
        bool hasPrev = false;
        AudioTiming current;
        double dpvrSum = 0;
        double iptSum = 0;
        uint32_t iptCount = 0;
        Histograms total;
        Histograms interval;
    };

    std::optional<AudioTiming> finishPeriod(PathState& state);

    AudioFormat format_;
    double packetNs_;
    double samplesPerPacket_;
    AudioPassCriteria pass_;
    std::array<PathState, kPathCount> paths_;
};

} // namespace st2110
