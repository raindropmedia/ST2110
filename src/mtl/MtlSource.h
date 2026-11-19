#pragma once

#include "core/Monitor.h"
#include "core/Options.h"

#include <memory>
#include <string>

namespace st2110 {

// Empfang über MTL: eine MTL-Instanz mit Port P (und R), je eine Sitzung für Video (st20, Bildebene)
// und Ton (st30, Bildebene), beide mit Timing-Parser. Liefert an den Monitor:
// - Bildwerte und Urteil von MTL (notify_frame_ready, st20_rx_tp_meta),
// - 200-ms-Werte für Ton (notify_timing_parser_result),
// - jedes Paket mit Hardware-Zeitstempel, wenn MTL den Patch aus patches/mtl/ enthält,
// - PTP-Korrekturen (ptp_sync_notify) und Sitzungszähler (alle 500 ms).
// Der Header bindet keine MTL-Header ein.
class MtlSource {
  public:
    static std::unique_ptr<MtlSource> create(const Options& options, Monitor& monitor, std::string& error);
    ~MtlSource();
    MtlSource(const MtlSource&) = delete;
    MtlSource& operator=(const MtlSource&) = delete;

    // true, wenn MTL mit dem Paket-Rückruf gebaut ist (Histogramme und Paketprofile verfügbar).
    static bool hasPacketHook();

  private:
    struct Impl;
    explicit MtlSource(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace st2110
