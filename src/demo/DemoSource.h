#pragma once

#include "core/Monitor.h"
#include "core/Options.h"
#include "core/SyntheticSender.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace st2110 {

// Synthetische Ströme ohne Karte: erzeugt Pakete in Echtzeit (Zeitbasis Systemuhr als TAI) und
// speist sie in den Monitor. Störungen lassen sich während des Laufs umschalten (Lehre, Vorführung).
class DemoSource {
  public:
    struct Preset {
        std::string id;
        std::string label; // deutsch, für die Oberfläche
        std::string description;
    };

    DemoSource(const Options& options, Monitor& monitor);
    ~DemoSource();
    DemoSource(const DemoSource&) = delete;
    DemoSource& operator=(const DemoSource&) = delete;

    static const std::vector<Preset>& presets();
    bool setPreset(const std::string& id);
    std::string preset() const;

  private:
    void run();
    void emitVideo(uint64_t frame, const SenderFaults& faults);
    void emitAudio(uint64_t packet, const SenderFaults& faults);

    Options options_;
    Monitor& monitor_;
    SyntheticSender videoP_;
    SyntheticSender videoR_;
    SyntheticSender audioP_;
    SyntheticSender audioR_;
    double ptpNoiseNs_ = 25;

    mutable std::mutex mutex_;
    std::string preset_ = "ideal";
    SenderFaults faults_;

    StreamCounters videoCounters_;
    StreamCounters audioCounters_;
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

} // namespace st2110
