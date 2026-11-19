#pragma once

#include "core/Options.h"
#include "core/TimingStats.h"

#include <chrono>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace st2110 {

// Ausgabe als JSON Lines: je Zeile ein Objekt mit "type" = "start" | "passCriteria" | "interval" |
// "summary".
// Feldnamen englisch (maschinenlesbar), Einheiten im Namen (Ns = Nanosekunden).

nlohmann::json rangeToJson(const Range& range);
nlohmann::json statsToJson(const TimingStats& stats);
nlohmann::json passCriteriaToJson(const PassCriteria& criteria);

// Formatname wie "1920x1080p50" bzw. "1920x1080i50".
std::string formatName(const Options& options);

// Startzeile: Einstellungen (ohne Ports und lokale Adressen) und, sobald bekannt, Grenzwerte.
nlohmann::json startRecord(const Options& options, const std::optional<PassCriteria>& criteria,
                           std::chrono::system_clock::time_point now);

// Grenzwerte des Timing-Parsers, sobald MTL sie nach dem ersten Bild kennt.
nlohmann::json passCriteriaRecord(const PassCriteria& criteria, std::chrono::system_clock::time_point now);

// Zeile je Intervall (type "interval") bzw. am Ende (type "summary"); redundant ist leer ohne ST 2022-7.
nlohmann::json intervalRecord(const std::string& type, unsigned intervalSec, const FrameCounters& frames,
                              const TimingStats& primary, const TimingStats* redundant,
                              std::chrono::system_clock::time_point now);

// UTC-Zeit als ISO 8601 mit Millisekunden ("2026-11-19T10:15:30.123Z").
std::string isoTimestamp(std::chrono::system_clock::time_point time);

} // namespace st2110
