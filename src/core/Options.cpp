#include "core/Options.h"

#include <arpa/inet.h>
#include <charconv>
#include <functional>
#include <limits>
#include <map>
#include <regex>
#include <sstream>

namespace st2110 {
namespace {

template <typename T> bool parseUnsigned(const std::string& text, T& out) {
    unsigned long long value = 0;
    const char* begin = text.data();
    const char* end = begin + text.size();
    auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end || text.empty()) {
        return false;
    }
    if (value > std::numeric_limits<T>::max()) {
        return false;
    }
    out = static_cast<T>(value);
    return true;
}

std::optional<LogLevel> parseLogLevel(const std::string& text) {
    static const std::map<std::string, LogLevel> kLevels{{"debug", LogLevel::Debug},
                                                         {"info", LogLevel::Info},
                                                         {"notice", LogLevel::Notice},
                                                         {"warning", LogLevel::Warning},
                                                         {"error", LogLevel::Error}};
    auto it = kLevels.find(text);
    if (it == kLevels.end()) {
        return std::nullopt;
    }
    return it->second;
}

void validatePath(const PathOptions& path, const std::string& label, const Options& options,
                  std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    if (path.port.empty()) {
        errors.push_back(label + ": Port fehlt (--port bzw. --red-port, PCI-Adresse oder kernel:<if>).");
    } else if (isKernelPort(path.port)) {
        if (options.hwTimestamp) {
            errors.push_back(label + ": kernel:-Ports liefern keine Hardware-Zeitstempel. Für einen reinen "
                                     "Empfangstest --no-hw-timestamp setzen (Messwerte dann unbrauchbar).");
        }
        if (options.ptp) {
            errors.push_back(label + ": Das eingebaute PTP von MTL braucht einen DPDK-Port (PF). Für "
                                     "kernel:-Ports --no-ptp setzen.");
        }
    } else if (!isPciAddress(path.port)) {
        errors.push_back(label + ": Port \"" + path.port +
                         "\" ist weder eine PCI-Adresse (0000:af:00.0) noch kernel:<if>.");
    }

    if (!isKernelPort(path.port) && !isIpv4Address(path.localIp)) {
        errors.push_back(label +
                         ": Eigene IPv4-Adresse fehlt oder ist ungültig (--local-ip bzw. --red-local-ip).");
    }
    if (!isIpv4Address(path.streamIp)) {
        errors.push_back(label + ": Adresse des Stroms fehlt oder ist ungültig (--ip bzw. --red-ip).");
    }
    if (!path.sourceIp.empty()) {
        if (!isIpv4Address(path.sourceIp)) {
            errors.push_back(label + ": Quelladresse \"" + path.sourceIp + "\" ist ungültig.");
        } else if (isIpv4Address(path.streamIp) && !isMulticastIpv4(path.streamIp)) {
            warnings.push_back(label + ": Quelladresse wird nur bei Multicast genutzt.");
        }
    }
    if (path.udpPort == 0) {
        errors.push_back(label + ": UDP-Port 0 ist ungültig.");
    }
}

} // namespace

bool isPciAddress(const std::string& text) {
    static const std::regex kPci("^[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\\.[0-7]$");
    return std::regex_match(text, kPci);
}

bool isKernelPort(const std::string& text) {
    return text.rfind("kernel:", 0) == 0 && text.size() > 7;
}

bool isIpv4Address(const std::string& text) {
    in_addr addr{};
    return !text.empty() && inet_pton(AF_INET, text.c_str(), &addr) == 1;
}

bool isMulticastIpv4(const std::string& text) {
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
        return false;
    }
    const uint32_t host = ntohl(addr.s_addr);
    return (host >> 28) == 0xE; // 224.0.0.0/4
}

void validateOptions(const Options& options, std::vector<std::string>& errors,
                     std::vector<std::string>& warnings) {
    validatePath(options.primary, "Primärpfad", options, errors, warnings);
    if (options.redundant) {
        validatePath(*options.redundant, "Redundanter Pfad", options, errors, warnings);
        if (options.redundant->port == options.primary.port) {
            errors.push_back("Primärer und redundanter Pfad nutzen denselben Port.");
        }
    }

    if (options.payloadType > 127) {
        errors.push_back("Payload-Typ muss zwischen 0 und 127 liegen.");
    } else if (options.payloadType < 96) {
        warnings.push_back("Payload-Typ unter 96: ST 2110 nutzt üblicherweise dynamische Typen (96–127).");
    }
    if (options.width < 64 || options.width > 16384 || options.width % 2 != 0) {
        errors.push_back("Breite muss gerade sein und zwischen 64 und 16384 liegen.");
    }
    if (options.height < 64 || options.height > 8704) {
        errors.push_back("Höhe muss zwischen 64 und 8704 liegen.");
    }
    if (options.interlaced && options.height % 2 != 0) {
        errors.push_back("Bei Zeilensprung muss die Höhe (volles Bild) gerade sein.");
    }
    if (options.intervalSec == 0) {
        errors.push_back("Berichtsabstand muss mindestens 1 s sein.");
    }

    if (!options.hwTimestamp) {
        warnings.push_back("Ohne Hardware-Zeitstempel misst MTL die Abholzeit der Software: Cinst, VRX, IPT "
                           "und FPT sind dann nicht aussagekräftig.");
    }
    if (!options.ptp) {
        warnings.push_back("Ohne PTP fehlt der gemeinsame Zeitbezug: FPT, Latenz und RTP-Offset sind dann "
                           "nicht aussagekräftig.");
    }
}

ParseResult parseOptions(const std::vector<std::string>& args) {
    ParseResult result;
    Options options;
    PathOptions red;
    bool redUsed = false;
    bool redUdpPortSet = false;

    using Setter = std::function<bool(const std::string&)>;
    const std::map<std::string, Setter> withValue{
        {"--port",
         [&](const std::string& v) {
             options.primary.port = v;
             return true;
         }},
        {"--local-ip",
         [&](const std::string& v) {
             options.primary.localIp = v;
             return true;
         }},
        {"--ip",
         [&](const std::string& v) {
             options.primary.streamIp = v;
             return true;
         }},
        {"--source-ip",
         [&](const std::string& v) {
             options.primary.sourceIp = v;
             return true;
         }},
        {"--udp-port", [&](const std::string& v) { return parseUnsigned(v, options.primary.udpPort); }},
        {"--red-port",
         [&](const std::string& v) {
             red.port = v;
             redUsed = true;
             return true;
         }},
        {"--red-local-ip",
         [&](const std::string& v) {
             red.localIp = v;
             redUsed = true;
             return true;
         }},
        {"--red-ip",
         [&](const std::string& v) {
             red.streamIp = v;
             redUsed = true;
             return true;
         }},
        {"--red-source-ip",
         [&](const std::string& v) {
             red.sourceIp = v;
             redUsed = true;
             return true;
         }},
        {"--red-udp-port",
         [&](const std::string& v) {
             redUsed = true;
             redUdpPortSet = true;
             return parseUnsigned(v, red.udpPort);
         }},
        {"--payload-type", [&](const std::string& v) { return parseUnsigned(v, options.payloadType); }},
        {"--width", [&](const std::string& v) { return parseUnsigned(v, options.width); }},
        {"--height", [&](const std::string& v) { return parseUnsigned(v, options.height); }},
        {"--fps",
         [&](const std::string& v) {
             auto rate = parseFrameRate(v);
             if (rate) {
                 options.rate = *rate;
             }
             return rate.has_value();
         }},
        {"--interval", [&](const std::string& v) { return parseUnsigned(v, options.intervalSec); }},
        {"--duration", [&](const std::string& v) { return parseUnsigned(v, options.durationSec); }},
        {"--lcores",
         [&](const std::string& v) {
             options.lcores = v;
             return !v.empty();
         }},
        {"--log-level",
         [&](const std::string& v) {
             auto level = parseLogLevel(v);
             if (level) {
                 options.logLevel = *level;
             }
             return level.has_value();
         }},
        {"--output",
         [&](const std::string& v) {
             options.outputPath = v;
             return !v.empty();
         }},
    };
    const std::map<std::string, std::function<void()>> flags{
        {"--interlaced", [&] { options.interlaced = true; }},
        {"--no-hw-timestamp", [&] { options.hwTimestamp = false; }},
        {"--no-ptp", [&] { options.ptp = false; }},
    };

    for (size_t i = 1; i < args.size(); ++i) {
        std::string name = args[i];
        std::optional<std::string> inlineValue;
        if (auto eq = name.find('='); name.rfind("--", 0) == 0 && eq != std::string::npos) {
            inlineValue = name.substr(eq + 1);
            name = name.substr(0, eq);
        }

        if (name == "-h" || name == "--help") {
            result.helpRequested = true;
            return result;
        }
        if (auto flag = flags.find(name); flag != flags.end()) {
            if (inlineValue) {
                result.errors.push_back("Schalter " + name + " erwartet keinen Wert.");
                continue;
            }
            flag->second();
            continue;
        }
        auto setter = withValue.find(name);
        if (setter == withValue.end()) {
            result.errors.push_back("Unbekannte Option: " + args[i]);
            continue;
        }
        std::string value;
        if (inlineValue) {
            value = *inlineValue;
        } else if (i + 1 < args.size()) {
            value = args[++i];
        } else {
            result.errors.push_back("Option " + name + " erwartet einen Wert.");
            continue;
        }
        if (!setter->second(value)) {
            std::string message = "Ungültiger Wert für " + name + ": \"" + value + "\"";
            if (name == "--fps") {
                message += " (erlaubt: " + frameRateNames() + ")";
            }
            result.errors.push_back(message + ".");
        }
    }

    if (redUsed) {
        if (!redUdpPortSet) {
            red.udpPort = options.primary.udpPort;
        }
        options.redundant = red;
    }

    if (!result.errors.empty()) {
        return result;
    }
    validateOptions(options, result.errors, result.warnings);
    if (result.errors.empty()) {
        result.options = options;
    }
    return result;
}

std::string usageText(const std::string& programName) {
    std::ostringstream out;
    out << "Aufruf: " << programName << " [Optionen]\n"
        << "\n"
        << "Empfängt einen ST-2110-20-Strom mit MTL und gibt je Intervall die Timing-Werte nach\n"
        << "ST 2110-21 als JSON Lines aus (Cinst, VRX, IPT, FPT, Latenz, RTP-Offset, Urteil).\n"
        << "\n"
        << "Pfad (Pflicht):\n"
        << "  --port <pci|kernel:if>   Port der Karte, z. B. 0000:af:00.0 (DPDK) oder kernel:enp1s0f0\n"
        << "  --local-ip <ipv4>        eigene Adresse am Port (bei DPDK Pflicht)\n"
        << "  --ip <ipv4>              Multicast-Gruppe oder Unicast-Adresse des Stroms\n"
        << "  --source-ip <ipv4>       Quelle für Source-Specific Multicast (optional)\n"
        << "  --udp-port <n>           UDP-Port des Stroms (Vorgabe 20000)\n"
        << "\n"
        << "Redundanz nach ST 2022-7 (optional):\n"
        << "  --red-port, --red-local-ip, --red-ip, --red-source-ip, --red-udp-port\n"
        << "\n"
        << "Format:\n"
        << "  --width <n> --height <n> (Vorgabe 1920x1080, Höhe immer volles Bild)\n"
        << "  --fps <rate>             " << frameRateNames() << " (Vorgabe 50;\n"
        << "                           bei Zeilensprung Halbbilder je Sekunde: 1080i50 = --fps 50)\n"
        << "  --interlaced             Zeilensprung\n"
        << "  --payload-type <n>       RTP-Payload-Typ (Vorgabe 112)\n"
        << "\n"
        << "Messung:\n"
        << "  --no-hw-timestamp        ohne Hardware-Zeitstempel (nur Funktionstest)\n"
        << "  --no-ptp                 ohne eingebautes PTP (nur Funktionstest)\n"
        << "  --interval <s>           Berichtsabstand (Vorgabe 1)\n"
        << "  --duration <s>           Messdauer, 0 = bis Strg+C (Vorgabe 0)\n"
        << "  --output <datei>         Berichte an Datei anhängen statt stdout\n"
        << "\n"
        << "Sonstiges:\n"
        << "  --lcores <liste>         CPU-Kerne für MTL, z. B. 4-7\n"
        << "  --log-level <stufe>      debug, info, notice, warning, error (Vorgabe info)\n"
        << "  -h, --help               diese Hilfe\n";
    return out.str();
}

} // namespace st2110
