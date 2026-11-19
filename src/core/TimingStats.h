#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <string>

namespace st2110 {

// Urteil des Timing-Parsers nach ST 2110-21 (entspricht enum st_rx_tp_compliant).
enum class Compliance { Failed = 0, Wide = 1, Narrow = 2 };

const char* complianceName(Compliance compliance);

// Messwerte eines Bildes (bzw. Halbbildes) auf einem Pfad. Unabhängig von MTL, damit Auswertung und
// Ausgabe ohne Karte testbar sind; der MTL-Teil kopiert die Felder aus struct st20_rx_tp_meta.
struct FrameTiming {
    int32_t cinstMin = 0;
    int32_t cinstMax = 0;
    float cinstAvg = 0;
    int32_t vrxMin = 0;
    int32_t vrxMax = 0;
    float vrxAvg = 0;
    int32_t iptMinNs = 0;
    int32_t iptMaxNs = 0;
    float iptAvgNs = 0;
    int32_t fptNs = 0;
    int32_t latencyNs = 0;
    int32_t rtpOffset = 0;  // Takte (90 kHz)
    int32_t rtpTsDelta = 0; // Takte (90 kHz)
    Compliance compliance = Compliance::Failed;
    std::string failedCause;
    uint32_t packets = 0;
};

// Minimum, Maximum und Mittelwert über ein Intervall.
class Range {
  public:
    void add(double minValue, double maxValue, double avgValue);
    void add(double value) { add(value, value, value); }
    // Übernimmt die Werte eines anderen Bereichs (Mittelwert gewichtet nach Anzahl).
    void merge(const Range& other);
    bool empty() const { return count_ == 0; }
    double min() const { return min_; }
    double max() const { return max_; }
    double mean() const { return count_ == 0 ? 0.0 : sum_ / static_cast<double>(count_); }
    uint64_t count() const { return count_; }

  private:
    double min_ = std::numeric_limits<double>::infinity();
    double max_ = -std::numeric_limits<double>::infinity();
    double sum_ = 0;
    uint64_t count_ = 0;
};

// Sammelt die Messwerte eines Pfades über ein Berichtsintervall.
class TimingStats {
  public:
    // Höchstzahl verschiedener Fehlerursachen je Intervall; weitere zählen unter kOtherCauses.
    static constexpr size_t kMaxCauses = 16;
    static constexpr const char* kOtherCauses = "(weitere Ursachen)";

    void add(const FrameTiming& frame);
    void reset() { *this = TimingStats{}; }
    // Fasst Intervalle zusammen (für die Zusammenfassung am Ende).
    void merge(const TimingStats& other);

    uint64_t frames() const { return frames_; }
    uint64_t packets() const { return packets_; }
    uint64_t count(Compliance compliance) const { return counts_[static_cast<size_t>(compliance)]; }
    // Schlechtestes Urteil im Intervall; Failed, wenn keine Bilder kamen.
    Compliance worst() const;

    const Range& cinst() const { return cinst_; }
    const Range& vrx() const { return vrx_; }
    const Range& iptNs() const { return ipt_; }
    const Range& fptNs() const { return fpt_; }
    const Range& latencyNs() const { return latency_; }
    const Range& rtpOffset() const { return rtpOffset_; }
    const Range& rtpTsDelta() const { return rtpTsDelta_; }
    const std::map<std::string, uint64_t>& failedCauses() const { return causes_; }

  private:
    void addCause(const std::string& cause, uint64_t count);

    uint64_t frames_ = 0;
    uint64_t packets_ = 0;
    std::array<uint64_t, 3> counts_{};
    Range cinst_;
    Range vrx_;
    Range ipt_;
    Range fpt_;
    Range latency_;
    Range rtpOffset_;
    Range rtpTsDelta_;
    std::map<std::string, uint64_t> causes_;
};

// Vollständigkeit der Bilder über alle Pfade (entspricht enum st_frame_status).
struct FrameCounters {
    uint64_t complete = 0;
    uint64_t reconstructed = 0; // Verluste über ST 2022-7 ausgeglichen
    uint64_t corrupted = 0;
    uint64_t dropped = 0;

    uint64_t total() const { return complete + reconstructed + corrupted + dropped; }
    void merge(const FrameCounters& other) {
        complete += other.complete;
        reconstructed += other.reconstructed;
        corrupted += other.corrupted;
        dropped += other.dropped;
    }
};

// Grenzwerte des Timing-Parsers (aus st20_rx_tp_pass), einmal je Strom.
struct PassCriteria {
    int32_t cinstMaxNarrow = 0;
    int32_t cinstMaxWide = 0;
    int32_t vrxMaxNarrow = 0;
    int32_t vrxMaxWide = 0;
    int32_t trOffsetNs = 0;
    int32_t latencyMaxNs = 0;
    int32_t latencyMinNs = 0;
    int32_t rtpOffsetMax = 0;
    int32_t rtpOffsetMin = 0;
    int32_t rtpTsDeltaMax = 0;
    int32_t rtpTsDeltaMin = 0;
};

} // namespace st2110
