#pragma once

#include "core/Options.h"
#include "core/TimingStats.h"

#include <memory>
#include <optional>
#include <string>

namespace st2110 {

// Empfängt einen ST-2110-20-Strom über MTL mit eingeschaltetem Timing-Parser
// (ST20P_RX_FLAG_TIMING_PARSER_META) und sammelt die Werte je Pfad.
//
// MTL lässt nur eine Instanz je Prozess sinnvoll zu (Einstellungen der Karte gelten für den ganzen
// Prozess); deshalb kapselt diese Klasse mtl_init/mtl_uninit und genau eine Sitzung.
// Der Header bindet keine MTL-Header ein, damit Kern und Tests ohne MTL bauen.
class St20TimingReceiver {
  public:
    struct Interval {
        FrameCounters frames;
        TimingStats primary;
        std::optional<TimingStats> redundant;
    };

    // Startet MTL und die Sitzung; liefert nullptr und eine Fehlermeldung bei Misserfolg.
    static std::unique_ptr<St20TimingReceiver> create(const Options& options, std::string& error);

    ~St20TimingReceiver();
    St20TimingReceiver(const St20TimingReceiver&) = delete;
    St20TimingReceiver& operator=(const St20TimingReceiver&) = delete;

    // Gibt die seit dem letzten Aufruf gesammelten Werte zurück und beginnt ein neues Intervall.
    Interval takeInterval();

    // Grenzwerte des Timing-Parsers; erst nach dem ersten empfangenen Bild bekannt.
    std::optional<PassCriteria> passCriteria() const;

    // Beendet die Sitzung (auch aus einem Signal-Handler-Kontext über ein Flag nutzbar, siehe main).
    void stop();

  private:
    struct Impl;
    explicit St20TimingReceiver(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace st2110
