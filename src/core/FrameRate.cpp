#include "core/FrameRate.h"

#include <array>

namespace st2110 {
namespace {

struct RateInfo {
    FrameRate rate;
    std::string_view name;
    double numerator;
    double denominator;
};

constexpr std::array<RateInfo, 11> kRates{{
    {FrameRate::P23_98, "23.98", 24000.0, 1001.0},
    {FrameRate::P24, "24", 24.0, 1.0},
    {FrameRate::P25, "25", 25.0, 1.0},
    {FrameRate::P29_97, "29.97", 30000.0, 1001.0},
    {FrameRate::P30, "30", 30.0, 1.0},
    {FrameRate::P50, "50", 50.0, 1.0},
    {FrameRate::P59_94, "59.94", 60000.0, 1001.0},
    {FrameRate::P60, "60", 60.0, 1.0},
    {FrameRate::P100, "100", 100.0, 1.0},
    {FrameRate::P119_88, "119.88", 120000.0, 1001.0},
    {FrameRate::P120, "120", 120.0, 1.0},
}};

const RateInfo& info(FrameRate rate) {
    for (const auto& entry : kRates) {
        if (entry.rate == rate) {
            return entry;
        }
    }
    return kRates.front(); // nicht erreichbar, alle Werte sind eingetragen
}

} // namespace

std::optional<FrameRate> parseFrameRate(std::string_view text) {
    for (const auto& entry : kRates) {
        if (entry.name == text) {
            return entry.rate;
        }
    }
    return std::nullopt;
}

std::string frameRateName(FrameRate rate) {
    return std::string(info(rate).name);
}

double framePeriodNs(FrameRate rate) {
    const auto& entry = info(rate);
    return 1e9 * entry.denominator / entry.numerator;
}

std::string frameRateNames() {
    std::string names;
    for (const auto& entry : kRates) {
        if (!names.empty()) {
            names += ", ";
        }
        names += entry.name;
    }
    return names;
}

} // namespace st2110
