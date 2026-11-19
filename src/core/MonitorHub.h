#pragma once

#include "core/FlowMonitor.h"
#include "core/FlowTable.h"
#include "core/PtpMonitor.h"
#include "core/SapDirectory.h"

#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace st2110 {

struct HubConfig {
    bool hwTimestamp = true;
    bool ptp = true;
    bool demo = false;
    unsigned maxFlows = 8;
    std::string discoverInterface; // nur zur Anzeige
};

// Führt alle überwachten Flows, PTP, das Ereignisprotokoll und die Flow-Erkennung (Mitlesen, SAP)
// zusammen. Threadsicher. Sperrreihenfolge: Flow vor Hub; der Hub ruft Flows nie unter seiner Sperre auf.
class MonitorHub {
  public:
    using Clock = std::function<double()>;

    explicit MonitorHub(HubConfig config, Clock clock = {});

    const HubConfig& config() const { return config_; }
    double now() const { return clock_() - startSec_; }

    // Legt einen Flow an (ID und Name werden vergeben, falls leer). Liefert nullptr und eine
    // Fehlermeldung bei ungültigen Angaben, doppelter Adresse oder erreichter Höchstzahl.
    std::shared_ptr<FlowMonitor> addFlow(FlowConfig config, std::string& error);
    bool removeFlow(const std::string& id);
    std::shared_ptr<FlowMonitor> flow(const std::string& id) const;
    std::vector<std::shared_ptr<FlowMonitor>> flows() const;

    void onPtpSync(int64_t deltaNs, int16_t utcOffset);
    void addEvent(const std::string& level, const std::string& text, const std::string& flowId = {});

    // Flow-Erkennung
    void onDiscoveredPacket(const PacketInfo& packet, uint32_t count = 1, uint64_t bytes = 0);
    void onSapMessage(const SapMessage& message);
    void setDiscoveryStatus(nlohmann::json status);
    // SDP einer Ankündigung (für "Überwachen"); leer, wenn unbekannt.
    std::string announcedSdp(const std::string& key) const;

    void reset();

    // Flow-Liste: überwachte Flows, erkannte Flows, SAP-Ankündigungen, Status der Erkennung.
    nlohmann::json flowsJson();
    // Zustand für die Oberfläche: global (PTP, Ereignisse, Flow-Liste) und Details eines Flows.
    nlohmann::json snapshot(const std::string& flowId, double windowSec = 60.0);
    nlohmann::json startRecord();
    nlohmann::json takeIntervalRecord();
    nlohmann::json summaryRecord();
    // Summe empfangener Bilder bzw. Ton-Zeiträume aller Flows.
    uint64_t received() const;

  private:
    struct Event {
        double t;
        std::string time;
        std::string level;
        std::string text;
        std::string flowId;
    };

    void addEventLocked(const std::string& level, const std::string& text, const std::string& flowId);
    void checkPtpLocked(double t);
    std::string nextIdLocked(Media media);
    nlohmann::json ptpJson(double windowSec);
    nlohmann::json eventsJsonLocked(size_t limit) const;
    std::string monitoredIdFor(const std::vector<std::shared_ptr<FlowMonitor>>& flows, uint32_t ip,
                               uint16_t port) const;

    HubConfig config_;
    Clock clock_;
    double startSec_;
    std::chrono::system_clock::time_point startWall_;
    mutable std::mutex mutex_;

    std::vector<std::shared_ptr<FlowMonitor>> flows_;
    unsigned nextVideo_ = 1;
    unsigned nextAudio_ = 1;

    PtpMonitor ptp_;
    PtpState lastPtpState_ = PtpState::NoSync;
    bool ptpStateKnown_ = false;

    std::deque<Event> events_;

    FlowTable discovered_;
    SapDirectory sap_;
    nlohmann::json discoveryStatus_;
    double lastExpire_ = 0;
};

} // namespace st2110
