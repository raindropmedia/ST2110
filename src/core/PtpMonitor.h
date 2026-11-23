#pragma once

#include "core/Histogram.h"
#include "core/TimeSeries.h"
#include "core/TimingStats.h"

#include <cstdint>
#include <deque>
#include <nlohmann/json.hpp>

namespace st2110 {

enum class PtpState { NoSync, Syncing, Locked };

const char* ptpStateName(PtpState state);

// Überwacht das eingebaute PTP von MTL über ptp_sync_notify: je Sync die Korrektur der Kartenuhr
// (delta, ns) und den UTC-Versatz des Grandmasters. MTL stellt weder Grandmaster-Kennung noch
// Pfadverzögerung bereit.
class PtpMonitor {
  public:
    struct Settings {
        // MTL stellt die Kartenuhr bei jedem Sync um den gemessenen Versatz zurück. Die gemeldete Korrektur
        // ist deshalb im Kern die Drift des Oszillators je Sync-Intervall (11 ppm × 125 ms ≈ 1,4 µs) und kein
        // Maß für die Güte. Bewertet wird die Streuung der Korrektur um ihren Mittelwert (Median der letzten
        // lockSamples Syncs): ein stabiler Regler liefert eine fast konstante Korrektur (hier ±30 ns).
        double lockThresholdNs = 500; // |Korrektur − Drift| darunter gilt als eingerastet
        size_t lockSamples = 8;       // so viele aufeinanderfolgende Syncs unter der Schwelle
        // Hysterese: Ein eingerasteter Zustand geht erst verloren, wenn ein Sync diese Abweichung erreicht.
        // Ohne sie meldete ein einzelner Ausreißer knapp über lockThresholdNs sofort "nicht eingerastet"
        // und nach acht Syncs wieder "eingerastet".
        double unlockThresholdNs = 1000;
        // Korrekturen ab dieser Größe sind Zeitsprünge (beim ersten Sync stellt MTL die Uhr um den
        // UTC-TAI-Versatz von 37 s). Sie zählen für sich und gehen nicht in Verlauf, Histogramm und Maximum
        // ein, sonst zeigt "max" bis zum Zurücksetzen 37 s.
        double stepThresholdNs = 1e6;
        double timeoutSec = 3.0; // ohne Sync länger als das: "kein Sync"
    };

    PtpMonitor();
    explicit PtpMonitor(Settings settings);

    void onSync(double nowSec, int64_t deltaNs, int16_t utcOffset);
    PtpState state(double nowSec) const;

    uint64_t syncCount() const { return syncs_; }
    int16_t utcOffset() const { return utcOffset_; }
    bool hasSync() const { return syncs_ > 0; }
    int64_t lastDeltaNs() const { return lastDelta_; }
    // Mittlere Korrektur je Sync (Median der letzten Syncs): die Drift des Oszillators.
    double driftNs() const { return drift_; }
    uint64_t steps() const { return steps_; }
    int64_t lastStepNs() const { return lastStep_; }
    double lastSyncSec() const { return lastSyncSec_; }

    const Range& totalDelta() const { return total_; }
    Range takeInterval();
    const Histogram& histogram() const { return histogram_; }
    const TimeSeries& series() const { return series_; }
    void resetTotals();

    nlohmann::json toJson(double nowSec, double windowSec, size_t maxPoints) const;

  private:
    Settings settings_;
    uint64_t syncs_ = 0;
    int16_t utcOffset_ = 0;
    int64_t lastDelta_ = 0;
    double lastSyncSec_ = 0;
    std::deque<int64_t> recent_;
    bool locked_ = false;
    double drift_ = 0;
    uint64_t steps_ = 0;
    int64_t lastStep_ = 0;
    Range total_;
    Range interval_;
    Histogram histogram_;
    TimeSeries series_{20000};
};

// Laufzeitunterschied zwischen den Pfaden nach ST 2022-7: Ankunft des ersten Pakets eines Bildes
// auf R minus P (ns, positiv = R später). Ordnet die Bilder über den RTP-Zeitstempel zu.
class PathSkew {
  public:
    PathSkew();
    // firstPacketNs: Ankunft des ersten Pakets bzw. FPT (beide Pfade gleich bezogen).
    void add(bool redundant, uint32_t rtpTimestamp, int64_t firstPacketNs, double nowSec);

    const Range& total() const { return total_; }
    Range takeInterval();
    const Histogram& histogram() const { return histogram_; }
    const TimeSeries& series() const { return series_; }
    bool hasValue() const { return hasLast_; }
    int64_t last() const { return last_; }
    void resetTotals();

  private:
    struct Pending {
        uint32_t rtp;
        int64_t ns;
    };
    void match(std::deque<Pending>& own, std::deque<Pending>& other, bool redundant, uint32_t rtp, int64_t ns,
               double nowSec);

    std::deque<Pending> primary_;
    std::deque<Pending> redundant_;
    Range total_;
    Range interval_;
    Histogram histogram_;
    TimeSeries series_{20000};
    int64_t last_ = 0;
    bool hasLast_ = false;
};

} // namespace st2110
