#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace st2110 {

// Bildraten, die MTL für ST 2110-20 kennt (enum st_fps). Bei Zeilensprung gilt die Halbbildrate:
// 1080i50 ist FrameRate::P50 mit interlaced = true.
enum class FrameRate { P23_98, P24, P25, P29_97, P30, P50, P59_94, P60, P100, P119_88, P120 };

// Liest "50", "59.94", "29.97" usw.; liefert nichts bei unbekannten Werten.
std::optional<FrameRate> parseFrameRate(std::string_view text);

// Kanonischer Name ("59.94").
std::string frameRateName(FrameRate rate);

// Bild- bzw. Halbbilddauer in Nanosekunden.
double framePeriodNs(FrameRate rate);

// Alle gültigen Namen, kommagetrennt (für Hilfetexte und Fehlermeldungen).
std::string frameRateNames();

} // namespace st2110
