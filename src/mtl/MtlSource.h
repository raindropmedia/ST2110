#pragma once

#include "core/FlowController.h"
#include "core/MonitorHub.h"
#include "core/Options.h"

#include <memory>
#include <string>

namespace st2110 {
class TestSender;
}

namespace st2110 {

// Empfang über MTL: eine MTL-Instanz mit Port P (und R) und je Flow eine Sitzung (st20 Bildebene bzw.
// st30), beide mit Timing-Parser. Flows lassen sich zur Laufzeit hinzufügen und entfernen; MTL
// reserviert dafür beim Start --max-flows Empfangs-Queues je Port. Liefert an den Hub bzw. die Flows:
// - Bildwerte und Urteil von MTL (notify_frame_ready, st20_rx_tp_meta),
// - 200-ms-Werte für Ton (notify_timing_parser_result),
// - jedes Paket mit Hardware-Zeitstempel, wenn MTL den Patch aus patches/mtl/ enthält,
// - PTP-Korrekturen (ptp_sync_notify) und Sitzungszähler (alle 500 ms).
// Der Header bindet keine MTL-Header ein.
class MtlSource : public FlowController {
  public:
    static std::unique_ptr<MtlSource> create(const Options& options, MonitorHub& hub, std::string& error);
    ~MtlSource() override;
    MtlSource(const MtlSource&) = delete;
    MtlSource& operator=(const MtlSource&) = delete;

    std::string addFlow(const FlowConfig& flow, std::string& error) override;
    bool removeFlow(const std::string& id, std::string& error) override;

    // Verwaltung der Testsender (nur mit --tx-port, sonst nullptr); gehört der Quelle.
    TestSender* testSender();

    // true, wenn MTL mit dem Paket-Rückruf gebaut ist (Histogramme und Paketprofile verfügbar).
    static bool hasPacketHook();

  private:
    struct Impl;
    explicit MtlSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace st2110
