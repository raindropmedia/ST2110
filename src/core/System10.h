#pragma once

#include <nlohmann/json.hpp>

namespace st2110 {

// Systemprüfung nach ST 2110-10 für einen Flow, zusammengesetzt aus Messwerten, die ohnehin vorliegen
// (PTP, RTP-Zeitstempel gegen PTP-Zeit, Zeitstempelschritt, Verluste, Reihenfolge, Payload-Typ, Herkunft der
// Zeitstempel). ST 2110-10 hat keine eigene Sitzungsart in MTL und keine eigenen Grenzwerte für den Verkehr;
// die Grenzen stammen, wo vorhanden, aus MTL (Video: st20_rx_tp_pass), sonst sind sie unten begründet.
//
// flow ist der Snapshot-Zweig "flow" des Hubs (FlowMonitor::snapshot), ptp der Zweig "ptp".
// Ergebnis: {"verdict": "ok"|"warn"|"fail"|"na", "checks": [{id, label, value, limit, verdict, note}]}.
nlohmann::json buildSystem10(const nlohmann::json& flow, const nlohmann::json& ptp, bool ptpEnabled,
                             bool hwTimestamp);

} // namespace st2110
