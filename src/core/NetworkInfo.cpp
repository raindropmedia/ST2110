#include "core/NetworkInfo.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace st2110 {

std::string parseDefaultRouteInterface(const std::string& procNetRoute) {
    // Kopfzeile: Iface Destination Gateway Flags RefCnt Use Metric Mask MTU Window IRTT (Tabulatoren)
    std::istringstream in(procNetRoute);
    std::string line;
    std::string best;
    long bestMetric = -1;
    bool first = true;
    while (std::getline(in, line)) {
        if (first) {
            first = false;
            if (line.rfind("Iface", 0) == 0) {
                continue;
            }
        }
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags, refCnt, use, metric, mask;
        if (!(fields >> iface >> destination >> gateway >> flags >> refCnt >> use >> metric >> mask)) {
            continue;
        }
        // Standardroute: Ziel und Maske 0, Route aktiv (RTF_UP = 1)
        if (std::strtoul(destination.c_str(), nullptr, 16) != 0 ||
            std::strtoul(mask.c_str(), nullptr, 16) != 0 ||
            (std::strtoul(flags.c_str(), nullptr, 16) & 1u) == 0) {
            continue;
        }
        const long value = std::strtol(metric.c_str(), nullptr, 10);
        if (bestMetric < 0 || value < bestMetric) {
            bestMetric = value;
            best = iface;
        }
    }
    return best;
}

std::string defaultRouteInterface() {
    std::ifstream file("/proc/net/route");
    if (!file) {
        return {};
    }
    std::stringstream content;
    content << file.rdbuf();
    return parseDefaultRouteInterface(content.str());
}

} // namespace st2110
