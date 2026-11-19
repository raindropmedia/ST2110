#pragma once

#include "core/AudioPacketAnalyzer.h"
#include "core/Options.h"
#include "core/PtpMonitor.h"
#include "core/TimeSeries.h"
#include "core/TimingStats.h"
#include "core/VideoPacketAnalyzer.h"

#include <array>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

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

// Wertet einen Flow aus und liefert Zustände für die Web-Oberfläche und die JSON-Berichte.
// Threadsicher: alle öffentlichen Funktionen sperren einen Mutex des Flows (die Paketfunktionen laufen
// im Datenpfad von MTL und halten ihn nur kurz).
//
// Bildwerte: Kommen Bilder aus MTL (onVideoFrame), zählen deren Timing-Werte; die eigene
// Paketanalyse liefert dann nur Histogramme und Profile. Ohne MTL-Bilder (Demo) zählen die
// Werte der eigenen Analyse.
class FlowMonitor {
  public:
    using Clock = std::function<double()>; // Sekunden, monoton
    // Ereignis an den Hub: Flow-ID, Stufe ("info" | "warn" | "error"), Text.
    using EventSink = std::function<void(const std::string&, const std::string&, const std::string&)>;

    explicit FlowMonitor(FlowConfig config, EventSink sink = {}, Clock clock = {});

    const FlowConfig& config() const { return config_; }
    const std::string& id() const { return config_.id; }

    void onVideoPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, int packetIndex,
                       bool hwTimestamp);
    void onVideoFrame(const VideoFrameReport& report);
    void setVideoPassCriteria(const PassCriteria& criteria);
    void setVideoPacketsPerFrame(int packets);

    void onAudioPacket(Path path, uint32_t rtpTimestamp, uint64_t arrivalNs, bool hwTimestamp);
    void onAudioPeriod(Path path, const AudioTiming& timing);

    void setCounters(const StreamCounters& counters);
    void addEvent(const std::string& level, const std::string& text);
    void reset();

    // Kurzform für die Flow-Liste: Adressen, Format, Urteil und Datenrate je Pfad.
    nlohmann::json summary();
    // Vollständiger Zustand des Flows (Verläufe der letzten windowSec Sekunden).
    nlohmann::json snapshot(double windowSec = 60.0);
    // Werte seit dem letzten Aufruf (für JSON Lines) bzw. über die ganze Messung.
    nlohmann::json takeIntervalRecord();
    nlohmann::json summaryRecord();

    // true, wenn Ziel (und, falls angegeben, Quelle) zu einem Pfad des Flows passen.
    bool matches(const std::string& ip, uint16_t port) const;
    // Empfangene Bilder bzw. Ton-Zeiträume (Rückgabewert des Programms).
    uint64_t received() const;

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
    struct CounterState {
        StreamCounters current;
        StreamCounters lastInterval;
        StreamCounters rateBase;
        double rateTime = 0;
        std::array<double, kPathCount> pps{};
        std::array<double, kPathCount> bps{};
        bool ratesValid = false;
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
        CounterState counters;
    };

    double now() const { return clock_(); }
    void addEventLocked(const std::string& level, const std::string& text);
    void recordVideoFrame(Path path, const FrameTiming& timing);
    void recordAudioPeriod(Path path, const AudioTiming& timing);
    nlohmann::json videoSnapshot(const SnapshotData& d, double windowSec) const;
    nlohmann::json audioSnapshot(const SnapshotData& d, double windowSec) const;
    nlohmann::json countersJson(const StreamCounters& counters) const;
    nlohmann::json countersDelta(const StreamCounters& now, const StreamCounters& before) const;
    nlohmann::json profileToJson(const PacketProfile& profile) const;
    nlohmann::json ratesJson(const CounterState& counters) const;
    std::optional<Compliance> verdictLocked(Path path) const;

    FlowConfig config_;
    EventSink sink_;
    Clock clock_;
    double startSec_;
    mutable std::mutex mutex_;

    std::unique_ptr<VideoPacketAnalyzer> videoAnalyzer_;
    std::optional<PassCriteria> mtlVideoPass_;
    bool mtlVideoFrames_ = false;
    std::array<VideoPath, kPathCount> video_;
    FrameCounters framesInterval_;
    FrameCounters framesTotal_;
    PathSkew videoSkew_;
    uint64_t videoPackets_ = 0;
    uint64_t videoSwPackets_ = 0;

    std::unique_ptr<AudioPacketAnalyzer> audioAnalyzer_;
    bool mtlAudioPeriods_ = false;
    std::array<AudioPath, kPathCount> audio_;
    uint64_t audioPackets_ = 0;
    uint64_t audioSwPackets_ = 0;

    CounterState counters_;
};

nlohmann::json flowAddressToJson(const FlowAddress& address);

} // namespace st2110
