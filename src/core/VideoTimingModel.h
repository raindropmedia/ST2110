#pragma once

#include "core/FrameRate.h"
#include "core/TimingStats.h"

#include <cstdint>
#include <string>

namespace st2110 {

// Kenngrößen nach ST 2110-21 für einen Videostrom (Sender-Modell "gapped").
// Die Formeln folgen bewusst dem Timing-Parser von MTL v26.09 (lib/src/st2110/st_rx_timing_parser.c,
// rv_tp_init), damit eigene Paketanalyse und MTL-Urteil dieselben Grenzen nutzen.
struct VideoTimingParams {
    double frameTimeNs = 0; // Bild- bzw. Halbbilddauer (TFRAME)
    double rActive = 0;     // Anteil aktiver Zeit (1080/1125, SD 487/525 bzw. 576/625)
    int packetsPerFrame = 0;
    double trsNs = 0;      // Sollabstand der Pakete (TRS = TFRAME * RACTIVE / NPACKETS)
    double trOffsetNs = 0; // Sollzeit des ersten Pakets nach Bildbeginn (TRO_DEFAULT)
    PassCriteria pass;
};

// Liefert die Kenngrößen; packetsPerFrame muss > 0 sein.
VideoTimingParams computeVideoTimingParams(uint32_t height, bool interlaced, FrameRate rate,
                                           int packetsPerFrame);

// Urteil für ein Bild wie in MTL (rv_tp_compliant): Reihenfolge der Prüfungen und Ursachentexte
// entsprechen MTL. cause erhält den Grund ("narrow" bei Erfolg).
Compliance evaluateVideoFrame(const FrameTiming& frame, const PassCriteria& pass, std::string& cause);

// Faktor, mit dem Cinst den Abfluss des Empfangspuffers berechnet (MTL: ST_TP_CINST_DRAIN_FACTOR).
inline constexpr double kCinstDrainFactor = 1.1;

} // namespace st2110
