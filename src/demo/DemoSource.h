#pragma once

#include "core/FlowController.h"
#include "core/MonitorHub.h"
#include "core/Options.h"
#include "core/SyntheticSender.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st2110 {

// Synthetische Flows ohne Karte: erzeugt Pakete in Echtzeit (Zeitbasis Systemuhr als TAI) und speist
// sie in die Flows des Hubs. Dazu simuliert sie die Erkennung (weitere Flows im Netz, PTP, Zusatzdaten)
// und SAP-Ankündigungen. Störungen lassen sich während des Laufs umschalten (Lehre, Vorführung).
class DemoSource : public FlowController {
  public:
    struct Preset {
        std::string id;
        std::string label; // deutsch, für die Oberfläche
        std::string description;
    };

    DemoSource(const Options& options, MonitorHub& hub);
    ~DemoSource() override;
    DemoSource(const DemoSource&) = delete;
    DemoSource& operator=(const DemoSource&) = delete;

    std::string addFlow(const FlowConfig& flow, std::string& error) override;
    bool removeFlow(const std::string& id, std::string& error) override;

    static const std::vector<Preset>& presets();
    bool setPreset(const std::string& id);
    std::string preset() const;

  private:
    struct Generator {
        std::shared_ptr<FlowMonitor> monitor; // leer: nur im Netz sichtbar (Erkennung)
        FlowConfig config;
        std::unique_ptr<SyntheticSender> senderP;
        std::unique_ptr<SyntheticSender> senderR;
        uint64_t next = 0; // nächste Epoche (Bild bzw. Tonpaket)
        bool started = false;
        StreamCounters counters;
        uint16_t sequence = 0;
        std::vector<float> toneBuffer;
        std::vector<uint8_t> hbrmtBuffer; // ein RTP-Paket nach ST 2022-6, Felder je Paket überschrieben
        uint64_t packetCounter = 0;
    };

    void run();
    void emitVideo(Generator& g, uint64_t frame, const SenderFaults& faults);
    void emitAudio(Generator& g, uint64_t packet, const SenderFaults& faults);
    void emitAnc(Generator& g, uint64_t frame, const SenderFaults& faults);
    void emitHbrmt(Generator& g, uint64_t frame, const SenderFaults& faults);
    void discoverVideo(const Generator& g, uint64_t frame, bool redundant, double skewNs);
    void discoverAudio(Generator& g, uint64_t firstPacket, uint32_t count);
    void announce();
    void previewLoop();
    void updatePreviewTargets();
    void previewAudio(Generator& g, uint64_t firstPacket, uint64_t packets);
    std::unique_ptr<Generator> makeGenerator(const FlowConfig& config, std::shared_ptr<FlowMonitor> monitor);

    MonitorHub& hub_;
    double ptpNoiseNs_ = 25;
    mutable std::mutex mutex_; // schützt preset_, faults_, generators_
    std::string preset_ = "ideal";
    SenderFaults faults_;
    std::vector<std::unique_ptr<Generator>> generators_;
    std::vector<FlowConfig> background_; // nur zur Erkennung
    uint64_t seed_ = 11;
    std::atomic<bool> stop_{false};
    std::thread thread_;
    std::thread previewThread_;

    struct PreviewTarget {
        std::shared_ptr<FlowMonitor> monitor;
        VideoFormat format;
        int index;
    };
    std::mutex previewMutex_; // schützt previewTargets_ (kurz gehalten)
    std::vector<PreviewTarget> previewTargets_;
    std::atomic<bool> glitch_{false};
};

} // namespace st2110
