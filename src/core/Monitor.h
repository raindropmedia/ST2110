#pragma once

#include "core/AudioPacketAnalyzer.h"
#include "core/Options.h"
#include "core/PtpMonitor.h"
#include "core/TimeSeries.h"
#include "core/TimingStats.h"
#include "core/VideoPacketAnalyzer.h"

#include <array>
#include <chrono>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace st2110 {

enum class FrameStatus { Complete, Reconstructed, Corrupted, Dropped };

// Ein Bild aus MTL (notify_frame_ready) mit den Werten des Timing-Parsers je Pfad.
struct VideoFrameReport {
    FrameStatus status = FrameStatus::Complete;
    uint32_t rtpTimestamp = 0;
    uint32_t packetsTotal = 0;
    std::array<uint32_t, kPathCount> packetsPerPath{};
    std::array<std::optional<FrameTiming>, kPathCount> timing;
};

// Zähler je Pfad und Strom (aus den Sitzungsstatistiken von MTL, kumulativ).
struct PathCounters {
    uint64_t packets = 0;
    uint64_t bytes = 0;
    uint64_t lost = 0;
    uint64_t reordered = 0;
    uint64_t duplicates = 0;
    uint64_t errors = 0;
};

struct StreamCounters {
    std::array<PathCounters, kPathCount> paths{};
    uint64_t received = 0;    // nach Redundanz angenommen
    uint64_t lost = 0;        // Summe der Verluste je Pfad (vor Redundanz)
    uint64_t unrecovered = 0; // nach Redundanz verloren
    uint64_t redundant = 0;   // Duplikate durch ST 2022-7
    uint64_t wrongPayloadType = 0;
    uint64_t wrongSsrc = 0;
    uint64_t framesIncomplete = 0;
};

enum class Media { Video, Audio };

struct MonitorConfig {
    std::optional<VideoFlow> video;
    std::optional<AudioFlow> audio;
    bool hwTimestamp = true;
    bool ptp = true;
    bool demo = false;
};

// Sammelt alle Messwerte, wertet sie aus und liefert Zustände für die Web-Oberfläche und die
// JSON-Berichte. Threadsicher: alle öffentlichen Funktionen sperren einen gemeinsamen Mutex
// (die Paketfunktionen laufen im Datenpfad von MTL und halten ihn nur kurz).
//
// Bildwerte: Kommen Bilder aus MTL (onVideoFrame), zählen deren Timing-Werte; die eigene
// Paketanalyse liefert dann nur Histogramme und Profile. Ohne MTL-Bilder (Demo, Ton) zählen die
// Werte der eigenen Analyse.
class Monitor {
  public:
    using Clock = std::function<double()>; // Sekunden, monoton

    explicit Monitor(MonitorConfig config, Clock clock = {});

    const MonitorConfig& config() const { return config_; }

    void onVideoPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, int packetIndex,
                       bool hwTimestamp);
    void onVideoFrame(const VideoFrameReport& report);
    void setVideoPassCriteria(const PassCriteria& criteria);
    void setVideoPacketsPerFrame(int packets);

    void onAudioPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, bool hwTimestamp);
    void onAudioPeriod(Path path, const AudioTiming& timing);

    void onPtpSync(int64_t deltaNs, int16_t utcOffset);
    void setCounters(Media media, const StreamCounters& counters);

    // Hinweis oder Fehler in das Ereignisprotokoll schreiben (z. B. von der Datenquelle).
    void addEvent(const std::string& level, const std::string& text);

    // Setzt Histogramme, Gesamtwerte, Profile und Ereignisse zurück (Taste "Zurücksetzen").
    void reset();

    // Vollständiger Zustand für die Web-Oberfläche (Verläufe der letzten windowSec Sekunden).
    nlohmann::json snapshot(double windowSec = 60.0);
    nlohmann::json startRecord();
    nlohmann::json takeIntervalRecord();
    nlohmann::json summaryRecord();

    uint64_t videoFrames() const;
    uint64_t audioPeriods() const;

  private:
    struct VideoPath {
        TimingStats interval;
        TimingStats total;
        TimingStats second; // laufende Sekunde für Ereignisse
        double secondStart = -1;
        std::optional<Compliance> lastSecondVerdict;
        std::optional<FrameTiming> lastFrame;
        TimeSeries cinstMax, vrxMin, vrxMax, fptUs, latencyUs, rtpOffset, iptMaxNs;
    };
    struct AudioPath {
        AudioStats interval;
        AudioStats total;
        std::optional<Compliance> lastVerdict;
        std::optional<AudioTiming> lastPeriod;
        TimeSeries dpvrMaxUs, dpvrMinUs, tsdfUs, iptMaxNs;
    };
    struct Event {
        double t;
        std::string time;
        std::string level;
        std::string text;
    };
    struct CounterState {
        StreamCounters current;
        StreamCounters lastInterval;
        bool any = false;
    };

    // Kopie aller Werte für snapshot(): wird unter der Sperre gefüllt, JSON entsteht danach ohne Sperre,
    // damit der Datenpfad von MTL nicht auf die Web-Oberfläche wartet.
    struct SnapshotData {
        double t = 0;
        std::array<VideoPath, kPathCount> video;
        std::array<AudioPath, kPathCount> audio;
        std::optional<PassCriteria> videoPass;
        std::optional<VideoTimingParams> videoParams;
        bool hasVideoAnalyzer = false;
        bool hasAudioAnalyzer = false;
        std::array<VideoPacketAnalyzer::Histograms, kPathCount> videoHist;
        std::array<PacketProfile, kPathCount> profileLast;
        std::array<PacketProfile, kPathCount> profileWorst;
        std::array<AudioPacketAnalyzer::Histograms, kPathCount> audioHist;
        AudioPassCriteria audioPass;
        bool mtlVideoFrames = false;
        bool mtlAudioPeriods = false;
        FrameCounters frames;
        uint64_t videoPackets = 0;
        uint64_t videoSwPackets = 0;
        uint64_t audioPackets = 0;
        uint64_t audioSwPackets = 0;
        PathSkew skew;
        TimeSeries skewSeries;
        PtpMonitor ptp;
        std::array<CounterState, 2> counters;
        std::vector<Event> events;
    };

    double now() const { return clock_(); }
    void addEventLocked(const std::string& level, const std::string& text);
    void recordVideoFrame(Path path, const FrameTiming& timing);
    void recordAudioPeriod(Path path, const AudioTiming& timing);
    void checkPtpLocked(double t);
    nlohmann::json videoSnapshot(const SnapshotData& d, double windowSec) const;
    nlohmann::json audioSnapshot(const SnapshotData& d, double windowSec) const;
    nlohmann::json countersJson(const StreamCounters& counters) const;
    nlohmann::json countersDelta(const StreamCounters& now, const StreamCounters& before) const;
    nlohmann::json profileToJson(const PacketProfile& profile) const;

    MonitorConfig config_;
    Clock clock_;
    std::chrono::system_clock::time_point startWall_;
    double startSec_;
    mutable std::mutex mutex_;

    std::unique_ptr<VideoPacketAnalyzer> videoAnalyzer_;
    std::optional<PassCriteria> mtlVideoPass_;
    bool mtlVideoFrames_ = false;
    std::array<VideoPath, kPathCount> video_;
    FrameCounters framesInterval_;
    FrameCounters framesTotal_;
    PathSkew videoSkew_;
    std::array<VideoPacketAnalyzer::Histograms, kPathCount> videoHistInterval_;
    uint64_t videoPackets_ = 0;
    uint64_t videoSwPackets_ = 0;

    std::unique_ptr<AudioPacketAnalyzer> audioAnalyzer_;
    bool mtlAudioPeriods_ = false;
    std::array<AudioPath, kPathCount> audio_;
    uint64_t audioPackets_ = 0;
    uint64_t audioSwPackets_ = 0;

    PtpMonitor ptp_;
    PtpState lastPtpState_ = PtpState::NoSync;
    bool ptpStateKnown_ = false;

    std::array<CounterState, 2> counters_;
    std::deque<Event> events_;
};

} // namespace st2110
