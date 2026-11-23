#pragma once

#include "core/MonitorHub.h"
#include "core/TestSender.h"

#include <chrono>
#include <thread>

namespace st2110 {

// Testsender für die Demo: sendet nichts, bildet aber den Ablauf nach (Start dauert 1,5 s wie das Warten
// auf PTP, Ereignisse im Hub), damit sich die Bedienung ohne Karte ausprobieren lässt.
class DemoTxBackend : public TxBackend {
  public:
    explicit DemoTxBackend(MonitorHub& hub) : hub_(hub) {}

    std::unique_ptr<Session> open(const FlowConfig& flow, std::string&) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        hub_.addEvent("info", "Testsender " + flow.label + " (Demo: es wird nichts gesendet) an " +
                                  flow.primary().ip + ":" + std::to_string(flow.primary().udpPort));
        return std::make_unique<Session>();
    }

  private:
    class Session : public TxBackend::Session {};
    MonitorHub& hub_;
};

} // namespace st2110
