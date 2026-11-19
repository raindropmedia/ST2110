#pragma once

#include "core/AudioPacketAnalyzer.h"
#include "core/Histogram.h"
#include "core/TimingStats.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <string>

namespace st2110 {

// Bausteine der JSON-Ausgabe. Feldnamen englisch (maschinenlesbar), Einheiten im Namen
// (Ns = Nanosekunden, Us = Mikrosekunden).

nlohmann::json rangeToJson(const Range& range);
nlohmann::json statsToJson(const TimingStats& stats);
nlohmann::json audioStatsToJson(const AudioStats& stats);
nlohmann::json passCriteriaToJson(const PassCriteria& criteria);
nlohmann::json audioPassToJson(const AudioPassCriteria& criteria);
// Kurzform eines Histogramms für Berichte: {count, min, p50, p99, max}
nlohmann::json histogramSummary(const Histogram& histogram);

// UTC-Zeit als ISO 8601 mit Millisekunden ("2026-11-19T10:15:30.123Z").
std::string isoTimestamp(std::chrono::system_clock::time_point time);

} // namespace st2110
