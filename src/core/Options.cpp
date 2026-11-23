#include "core/Options.h"

#include <arpa/inet.h>
#include <charconv>
#include <fstream>
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
    if (ec != std::errc{} || ptr != end || text.empty() || value > std::numeric_limits<T>::max()) {
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
    return it == kLevels.end() ? std::nullopt : std::optional<LogLevel>(it->second);
}

// "L24/48000/2"
bool parseAudioEncoding(const std::string& text, AudioFormat& format) {
    static const std::regex kEnc(R"(^L(16|24)/(\d+)/(\d+)$)");
    std::smatch m;
    if (!std::regex_match(text, m, kEnc)) {
        return false;
    }
    format.bitDepth = static_cast<uint8_t>(std::stoi(m[1].str()));
    return parseUnsigned(m[2].str(), format.sampleRate) && parseUnsigned(m[3].str(), format.channels);
}

void validatePort(const PortOptions& port, const std::string& label, const Options& options,
                  std::vector<std::string>& errors) {
    if (port.port.empty()) {
        errors.push_back(label + ": Port fehlt (PCI-Adresse oder kernel:<if>).");
        return;
    }
    if (isKernelPort(port.port)) {
        if (options.hwTimestamp) {
            errors.push_back(label + ": kernel:-Ports liefern keine Hardware-Zeitstempel. Für einen reinen "
                                     "Empfangstest --no-hw-timestamp setzen (Messwerte dann unbrauchbar).");
        }
        if (options.ptp) {
            errors.push_back(label + ": Das eingebaute PTP von MTL braucht einen DPDK-Port (PF). Für "
                                     "kernel:-Ports --no-ptp setzen.");
        }
        return;
    }
    if (!isPciAddress(port.port)) {
        errors.push_back(label + ": Port \"" + port.port +
                         "\" ist weder eine PCI-Adresse (0000:af:00.0) noch kernel:<if>.");
    }
    if (!isIpv4Address(port.localIp)) {
        errors.push_back(label + ": Eigene IPv4-Adresse fehlt oder ist ungültig.");
    }
}

void validateAddress(const FlowAddress& flow, const std::string& label, std::vector<std::string>& errors,
                     std::vector<std::string>& warnings) {
    if (!isIpv4Address(flow.ip)) {
        errors.push_back(label + ": Adresse fehlt oder ist ungültig.");
    }
    if (flow.udpPort == 0) {
        errors.push_back(label + ": UDP-Port fehlt.");
    }
    if (!flow.sourceIp.empty()) {
        if (!isIpv4Address(flow.sourceIp)) {
            errors.push_back(label + ": Quelladresse \"" + flow.sourceIp + "\" ist ungültig.");
        } else if (isIpv4Address(flow.ip) && !isMulticastIpv4(flow.ip)) {
            warnings.push_back(label + ": Quelladresse wird nur bei Multicast genutzt.");
        }
    }
}

FlowAddress fromSdp(const SdpFlow& f) {
    return FlowAddress{f.ip, f.port, f.sourceIp};
}

} // namespace

bool readTextFile(const std::string& path, std::string& content) {
    std::ifstream in(path);
    if (!in) {
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    content = buffer.str();
    return true;
}

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
    return (ntohl(addr.s_addr) >> 28) == 0xE; // 224.0.0.0/4
}

std::optional<FlowAddress> parseFlowAddress(const std::string& text) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        return std::nullopt;
    }
    FlowAddress flow;
    flow.ip = text.substr(0, colon);
    if (!isIpv4Address(flow.ip) || !parseUnsigned(text.substr(colon + 1), flow.udpPort) ||
        flow.udpPort == 0) {
        return std::nullopt;
    }
    return flow;
}

bool parseListen(const std::string& text, std::string& host, uint16_t& port) {
    const auto colon = text.rfind(':');
    if (colon == std::string::npos) {
        return false;
    }
    host = text.substr(0, colon);
    return (host == "0.0.0.0" || isIpv4Address(host)) && parseUnsigned(text.substr(colon + 1), port) &&
           port != 0;
}

std::string FlowConfig::formatName() const {
    return video ? video->format.name() : audio->format.name();
}

bool parseFlowSpec(Media media, const std::string& text, FlowConfig& flow, std::string& error) {
    std::vector<std::string> parts;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        parts.push_back(item);
    }
    if (parts.empty()) {
        error = "Leere Flow-Angabe.";
        return false;
    }
    auto address = parseFlowAddress(parts[0]);
    if (!address) {
        error = "Adresse \"" + parts[0] + "\" ungültig (Form 239.1.1.1:20000).";
        return false;
    }
    VideoFlow video;
    AudioFlow audio;
    std::optional<FlowAddress> red;
    std::string source, redSource;
    for (size_t i = 1; i < parts.size(); ++i) {
        const auto eq = parts[i].find('=');
        if (eq == std::string::npos) {
            error = "Angabe \"" + parts[i] + "\" ohne Wert (Form schlüssel=wert).";
            return false;
        }
        const std::string key = parts[i].substr(0, eq);
        const std::string value = parts[i].substr(eq + 1);
        bool ok = true;
        if (key == "red") {
            red = parseFlowAddress(value);
            ok = red.has_value();
        } else if (key == "source") {
            source = value;
        } else if (key == "red-source") {
            redSource = value;
        } else if (key == "name") {
            flow.label = value;
        } else if (key == "pt") {
            ok = parseUnsigned(value, media == Media::Video ? video.payloadType : audio.payloadType);
        } else if (key == "format" && media == Media::Video) {
            auto f = parseVideoFormat(value);
            ok = f.has_value();
            if (ok) {
                video.format = *f;
            }
        } else if (key == "format" && media == Media::Audio) {
            ok = parseAudioEncoding(value, audio.format);
        } else if (key == "ptime" && media == Media::Audio) {
            auto p = parsePacketTime(value);
            ok = p.has_value();
            if (ok) {
                audio.format.ptime = *p;
            }
        } else {
            error = "Unbekannte Angabe \"" + key + "\".";
            return false;
        }
        if (!ok) {
            error = "Ungültiger Wert für " + key + ": \"" + value + "\"";
            if (key == "format") {
                error += media == Media::Video ? " (z. B. 1920x1080p50; Raten: " + frameRateNames() + ")"
                                               : " (z. B. L24/48000/2)";
            } else if (key == "ptime") {
                error += " (0.125, 0.25, 0.333, 1 oder 4)";
            }
            error += ".";
            return false;
        }
    }
    address->sourceIp = source;
    if (red) {
        red->sourceIp = redSource;
    }
    if (media == Media::Video) {
        video.primary = *address;
        video.redundant = red;
        flow.video = video;
        flow.audio.reset();
    } else {
        audio.primary = *address;
        audio.redundant = red;
        flow.audio = audio;
        flow.video.reset();
    }
    return true;
}

std::vector<FlowConfig> flowsFromSdp(const std::string& sdpText, std::vector<std::string>& errors) {
    std::vector<FlowConfig> out;
    std::string error;
    auto session = parseSdp(sdpText, error);
    if (!session) {
        errors.push_back(error);
        return out;
    }
    const auto pairs = sdpMediaPairs(*session);
    size_t mediaCount = 0;
    for (const auto& [p, r] : pairs) {
        if (p->type == "video" || p->type == "audio") {
            ++mediaCount;
        }
    }
    for (const auto& [p, r] : pairs) {
        FlowConfig flow;
        std::string merror;
        const std::string name =
            session->name.empty() || session->name == "-" ? std::string("SDP") : session->name;
        if (p->type == "video") {
            auto v = sdpVideoFrom(*p, r, merror);
            if (!v) {
                errors.push_back(merror);
                continue;
            }
            VideoFlow video;
            video.primary = fromSdp(v->primary);
            if (v->redundant) {
                video.redundant = fromSdp(*v->redundant);
            }
            video.format = v->format;
            video.payloadType = static_cast<uint8_t>(v->payloadType);
            flow.video = video;
            flow.label = mediaCount > 1 ? name + " Video" : name;
        } else if (p->type == "audio") {
            auto a = sdpAudioFrom(*p, r, merror);
            if (!a) {
                errors.push_back(merror);
                continue;
            }
            AudioFlow audio;
            audio.primary = fromSdp(a->primary);
            if (a->redundant) {
                audio.redundant = fromSdp(*a->redundant);
            }
            audio.format = a->format;
            audio.payloadType = static_cast<uint8_t>(a->payloadType);
            flow.audio = audio;
            flow.label = mediaCount > 1 ? name + " Ton" : name;
        } else {
            continue; // z. B. ST 2110-40 (Zusatzdaten): noch nicht unterstützt
        }
        out.push_back(flow);
    }
    // Gleiche Namen unterscheidbar machen.
    std::map<std::string, int> seen;
    for (auto& f : out) {
        if (++seen[f.label] > 1) {
            f.label += " " + std::to_string(seen[f.label]);
        }
    }
    return out;
}

void validateFlow(const FlowConfig& flow, std::vector<std::string>& errors,
                  std::vector<std::string>& warnings) {
    const std::string name =
        flow.label.empty() ? (flow.video ? std::string("Video") : std::string("Ton")) : flow.label;
    if (flow.video.has_value() == flow.audio.has_value()) {
        errors.push_back(name + ": Flow muss genau Video oder Ton sein.");
        return;
    }
    validateAddress(flow.primary(), name + " P", errors, warnings);
    if (flow.redundant()) {
        validateAddress(*flow.redundant(), name + " R", errors, warnings);
    }
    if (flow.payloadType() > 127) {
        errors.push_back(name + ": Payload-Typ muss zwischen 0 und 127 liegen.");
    }
    if (flow.video) {
        const auto& f = flow.video->format;
        if (f.width < 64 || f.width > 16384 || f.width % 2 != 0) {
            errors.push_back(name + ": Breite muss gerade sein und zwischen 64 und 16384 liegen.");
        }
        if (f.height < 64 || f.height > 8704 || (f.interlaced && f.height % 2 != 0)) {
            errors.push_back(name + ": Höhe muss zwischen 64 und 8704 liegen (bei Zeilensprung gerade).");
        }
    } else if (auto problem = validateAudioFormat(flow.audio->format); !problem.empty()) {
        errors.push_back(name + ": " + problem);
    }
}

void validateOptions(const Options& o, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    // Mit Port darf der Start ohne Flows erfolgen: Flows und Testsender lassen sich in der Oberfläche
    // anlegen.
    if (o.flows.empty() && o.discoverInterface.empty() && o.txFlows.empty() && o.primaryPort.port.empty() &&
        !o.demo) {
        errors.push_back("Kein Flow angegeben (--video, --audio, --sdp), keine Erkennung (--discover) und "
                         "kein Port (--port): nichts zu tun.");
    }
    if (o.maxFlows == 0 || o.maxFlows > 64) {
        errors.push_back("--max-flows muss zwischen 1 und 64 liegen.");
    } else if (o.flows.size() > o.maxFlows) {
        errors.push_back("Mehr Flows (" + std::to_string(o.flows.size()) + ") als --max-flows (" +
                         std::to_string(o.maxFlows) + ").");
    }
    bool anyRedundant = false;
    for (const auto& f : o.flows) {
        validateFlow(f, errors, warnings);
        anyRedundant = anyRedundant || f.redundant().has_value();
    }
    // Reine Erkennung (Netz-Scanner) braucht keinen Port der Karte.
    const bool scannerOnly =
        o.flows.empty() && o.txFlows.empty() && !o.discoverInterface.empty() && o.primaryPort.port.empty();
    if (!o.demo && !scannerOnly) {
        validatePort(o.primaryPort, "Port P", o, errors);
        if (o.redundantPort) {
            validatePort(*o.redundantPort, "Port R", o, errors);
            if (o.redundantPort->port == o.primaryPort.port) {
                errors.push_back("Port P und Port R sind derselbe Port.");
            }
        }
        if (anyRedundant && !o.redundantPort) {
            errors.push_back("Redundanter Flow ohne redundanten Port (--red-port, --red-local-ip).");
        }
    }
    if (o.maxTxFlows == 0 || o.maxTxFlows > 16) {
        errors.push_back("--max-tx-flows muss zwischen 1 und 16 liegen.");
    } else if (o.txFlows.size() > o.maxTxFlows) {
        errors.push_back("Mehr Testsender (" + std::to_string(o.txFlows.size()) + ") als --max-tx-flows (" +
                         std::to_string(o.maxTxFlows) + ").");
    }
    if (o.txQueues > 64) {
        errors.push_back("--tx-queues muss zwischen 1 und 64 liegen.");
    }
    if (!o.txFlows.empty() || o.txPort) {
        if (o.demo) {
            errors.push_back("Der Testsender braucht die Karte (nicht mit --demo).");
        } else if (!o.txPort) {
            errors.push_back("Testsender ohne Sendeport (--tx-port, --tx-local-ip).");
        } else {
            validatePort(*o.txPort, "Sendeport", o, errors);
            if (o.txPort->port == o.primaryPort.port ||
                (o.redundantPort && o.txPort->port == o.redundantPort->port)) {
                errors.push_back("Der Sendeport muss ein anderer Port als P und R sein.");
            }
            if (isKernelPort(o.txPort->port)) {
                errors.push_back(
                    "Der Testsender braucht einen DPDK-Port (PCI-Adresse), keinen kernel:-Port.");
            }
        }
        for (const auto& f : o.txFlows) {
            validateFlow(f, errors, warnings);
            if (f.redundant()) {
                errors.push_back((f.label.empty() ? f.primary().ip : f.label) +
                                 ": Der Testsender sendet ohne ST 2022-7 (kein red=).");
            }
        }
    }
    if (o.intervalSec == 0) {
        errors.push_back("Berichtsabstand muss mindestens 1 s sein.");
    }
    if (!o.httpListen.empty()) {
        std::string host;
        uint16_t port = 0;
        if (!parseListen(o.httpListen, host, port)) {
            errors.push_back("--http erwartet adresse:port, z. B. 127.0.0.1:8090.");
        } else if (host != "127.0.0.1") {
            warnings.push_back("Web-Oberfläche ist ohne Anmeldung von außen erreichbar (" + o.httpListen +
                               ").");
        }
    }
    if (!o.demo && !o.hwTimestamp) {
        warnings.push_back("Ohne Hardware-Zeitstempel misst MTL die Abholzeit der Software: Cinst, VRX, IPT "
                           "und FPT sind dann nicht aussagekräftig.");
    }
    if (!o.demo && !o.ptp) {
        warnings.push_back("Ohne PTP fehlt der gemeinsame Zeitbezug: FPT, Latenz und RTP-Offset sind dann "
                           "nicht aussagekräftig.");
    }
}

namespace {

std::vector<FlowConfig> demoFlows() {
    std::vector<FlowConfig> flows;
    std::string error;
    const std::pair<Media, const char*> specs[] = {
        {Media::Video, "239.100.0.1:20000,red=239.101.0.1:20000,format=1920x1080p50,name=Kamera 1"},
        {Media::Audio,
         "239.100.0.2:30000,red=239.101.0.2:30000,format=L24/48000/2,ptime=1,name=Kamera 1 Ton"},
        {Media::Video, "239.100.0.11:20000,format=1920x1080i50,name=Kamera 2"},
        {Media::Audio, "239.100.0.12:30000,format=L24/48000/8,ptime=0.125,name=Mischpult"},
    };
    for (const auto& [media, spec] : specs) {
        FlowConfig f;
        parseFlowSpec(media, spec, f, error);
        flows.push_back(f);
    }
    return flows;
}

} // namespace

ParseResult parseOptions(const std::vector<std::string>& args, FileReader readFile) {
    ParseResult result;
    Options o;
    PortOptions redPort;
    bool redPortSet = false;
    PortOptions txPort;
    bool txPortSet = false;
    std::vector<std::pair<Media, std::string>> txSpecs;
    std::vector<std::string> sdpFiles;
    std::vector<std::pair<Media, std::string>> flowSpecs;

    using Setter = std::function<bool(const std::string&)>;
    const std::map<std::string, Setter> withValue{
        {"--port",
         [&](const std::string& v) {
             o.primaryPort.port = v;
             return true;
         }},
        {"--local-ip",
         [&](const std::string& v) {
             o.primaryPort.localIp = v;
             return true;
         }},
        {"--red-port",
         [&](const std::string& v) {
             redPort.port = v;
             redPortSet = true;
             return true;
         }},
        {"--red-local-ip",
         [&](const std::string& v) {
             redPort.localIp = v;
             redPortSet = true;
             return true;
         }},
        {"--tx-port",
         [&](const std::string& v) {
             txPort.port = v;
             txPortSet = true;
             return true;
         }},
        {"--tx-local-ip",
         [&](const std::string& v) {
             txPort.localIp = v;
             txPortSet = true;
             return true;
         }},
        {"--max-tx-flows", [&](const std::string& v) { return parseUnsigned(v, o.maxTxFlows); }},
        {"--tx-queues", [&](const std::string& v) { return parseUnsigned(v, o.txQueues); }},
        {"--tx-video",
         [&](const std::string& v) {
             txSpecs.emplace_back(Media::Video, v);
             return !v.empty();
         }},
        {"--tx-audio",
         [&](const std::string& v) {
             txSpecs.emplace_back(Media::Audio, v);
             return !v.empty();
         }},
        {"--video",
         [&](const std::string& v) {
             flowSpecs.emplace_back(Media::Video, v);
             return !v.empty();
         }},
        {"--audio",
         [&](const std::string& v) {
             flowSpecs.emplace_back(Media::Audio, v);
             return !v.empty();
         }},
        {"--sdp",
         [&](const std::string& v) {
             sdpFiles.push_back(v);
             return !v.empty();
         }},
        {"--max-flows", [&](const std::string& v) { return parseUnsigned(v, o.maxFlows); }},
        {"--discover",
         [&](const std::string& v) {
             o.discoverInterface = v;
             return !v.empty();
         }},
        {"--interval", [&](const std::string& v) { return parseUnsigned(v, o.intervalSec); }},
        {"--duration", [&](const std::string& v) { return parseUnsigned(v, o.durationSec); }},
        {"--lcores",
         [&](const std::string& v) {
             o.lcores = v;
             return !v.empty();
         }},
        {"--log-level",
         [&](const std::string& v) {
             auto level = parseLogLevel(v);
             if (level) {
                 o.logLevel = *level;
             }
             return level.has_value();
         }},
        {"--output",
         [&](const std::string& v) {
             o.outputPath = v;
             return !v.empty();
         }},
        {"--http",
         [&](const std::string& v) {
             o.httpListen = v;
             return !v.empty();
         }},
        {"--webui",
         [&](const std::string& v) {
             o.webuiDir = v;
             return !v.empty();
         }},
    };
    const std::map<std::string, std::function<void()>> flags{
        {"--demo", [&] { o.demo = true; }},   {"--no-hw-timestamp", [&] { o.hwTimestamp = false; }},
        {"--no-ptp", [&] { o.ptp = false; }}, {"--no-http", [&] { o.httpListen.clear(); }},
        {"--no-sap", [&] { o.sap = false; }}, {"--no-discover", [&] { o.autoDiscover = false; }},
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
            } else {
                flag->second();
            }
            continue;
        }
        auto setter = withValue.find(name);
        if (setter == withValue.end()) {
            std::string message = "Unbekannte Option: " + args[i];
            if (name.rfind("--video-", 0) == 0 || name.rfind("--audio-", 0) == 0) {
                message += " (Format, Redundanz usw. jetzt als Teil von --video/--audio, z. B. "
                           "--video 239.1.1.1:20000,format=1920x1080p50,red=239.2.1.1:20000)";
            }
            result.errors.push_back(message);
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
            result.errors.push_back("Ungültiger Wert für " + name + ": \"" + value + "\".");
        }
    }

    for (const auto& [media, spec] : flowSpecs) {
        FlowConfig flow;
        std::string error;
        if (!parseFlowSpec(media, spec, flow, error)) {
            result.errors.push_back((media == Media::Video ? "--video " : "--audio ") + spec + ": " + error);
            continue;
        }
        o.flows.push_back(flow);
    }
    for (const auto& [media, spec] : txSpecs) {
        FlowConfig flow;
        std::string error;
        if (!parseFlowSpec(media, spec, flow, error)) {
            result.errors.push_back((media == Media::Video ? "--tx-video " : "--tx-audio ") + spec + ": " +
                                    error);
            continue;
        }
        if (flow.label.empty()) {
            flow.label = media == Media::Video ? "Testsender Video" : "Testsender Ton";
        }
        o.txFlows.push_back(flow);
    }
    for (const auto& file : sdpFiles) {
        std::string text;
        if (!readFile(file, text)) {
            result.errors.push_back("SDP-Datei kann nicht gelesen werden: " + file);
            continue;
        }
        std::vector<std::string> errors;
        for (auto& flow : flowsFromSdp(text, errors)) {
            o.flows.push_back(flow);
        }
        for (const auto& e : errors) {
            result.errors.push_back(file + ": " + e);
        }
    }
    if (o.demo && o.flows.empty()) {
        o.flows = demoFlows();
    }
    if (redPortSet) {
        o.redundantPort = redPort;
    }
    if (txPortSet) {
        o.txPort = txPort;
    }

    if (!result.errors.empty()) {
        return result;
    }
    validateOptions(o, result.errors, result.warnings);
    if (result.errors.empty()) {
        result.options = o;
    }
    return result;
}

unsigned txQueueCount(unsigned maxTxFlows, unsigned override) {
    if (override > 0) {
        return override;
    }
    return std::min(2 * maxTxFlows + 1, kAutoTxQueueLimit);
}

std::string usageText(const std::string& programName) {
    std::ostringstream out;
    out << "Aufruf: " << programName << " [Optionen]\n"
        << "\n"
        << "Messgerät für SMPTE ST 2110 auf Basis von MTL: Video (ST 2110-20/-21), Ton (ST 2110-30),\n"
        << "PTP und ST 2022-7 für mehrere Flows; Erkennung aller Flows im Netz. Web-Oberfläche mit\n"
        << "Flow-Liste, Histogrammen und Verläufen, Berichte als JSON Lines.\n"
        << "\n"
        << "Ports der Karte:\n"
        << "  --port <pci|kernel:if>      Port P, z. B. 0000:af:00.0 (PF von Port 0 für PTP)\n"
        << "  --local-ip <ipv4>           eigene Adresse an Port P\n"
        << "  --red-port, --red-local-ip  Port R für ST 2022-7\n"
        << "\n"
        << "Flows (mehrfach möglich, weitere zur Laufzeit über die Oberfläche):\n"
        << "  --video <ip:port[,…]>       Schlüssel: red=ip:port, format=1920x1080p50|1920x1080i50,\n"
        << "                              pt=96, source=ip, red-source=ip, name=Text\n"
        << "  --audio <ip:port[,…]>       Schlüssel: red=ip:port, format=L24/48000/2, ptime=1 (0.125, 0.25,\n"
        << "                              0.333, 4), pt=97, source=ip, red-source=ip, name=Text\n"
        << "  --sdp <datei>               alle Video- und Tonmedien der SDP (DUP-Gruppen ergeben P und R)\n"
        << "  --max-flows <n>             Höchstzahl gleichzeitiger Flows (Vorgabe 8)\n"
        << "\n"
        << "Testsender (MTL, eigener Port derselben Karte, Zeitbezug über PTP wie beim Empfang):\n"
        << "  --tx-port, --tx-local-ip    Sendeport, z. B. 0000:af:00.1, und eigene Adresse daran\n"
        << "  --max-tx-flows <n>          Höchstzahl der Testsender (Vorgabe 4), weitere legt die Oberfläche "
           "an\n"
        << "  --tx-queues <n>             Sende-Queues des Sendeports (Vorgabe: 2 je Sender, höchstens 7;\n"
        << "                              die E830 startete mit 9 nicht)\n"
        << "  --tx-video <ip:port[,…]>    Testbild (Farbbalken, laufendes Quadrat, Sekunden-Blitz);\n"
        << "                              Schlüssel: format, pt, name\n"
        << "  --tx-audio <ip:port[,…]>    Testton (1 kHz links mit EBU-Kennung, 2 kHz rechts, −18 dBFS);\n"
        << "                              Schlüssel: format, ptime, pt, name\n"
        << "\n"
        << "Erkennung:\n"
        << "  --discover <schnittstelle>  alle UDP/RTP-Flows auf einer Kernel-Schnittstelle mitlesen\n"
        << "                              (Spiegelport o. Ä.; braucht CAP_NET_RAW) und SAP empfangen\n"
        << "                              Ohne Angabe liest das Programm im DPDK-Betrieb auf dem\n"
        << "                              Verwaltungsport (Schnittstelle mit der Standardroute) mit.\n"
        << "  --no-discover               keine automatische Erkennung auf dem Verwaltungsport\n"
        << "  --no-sap                    keine SAP-Ankündigungen empfangen\n"
        << "\n"
        << "Messung und Ausgabe:\n"
        << "  --demo                      synthetische Flows ohne Karte (Vorführung, Test)\n"
        << "  --no-hw-timestamp, --no-ptp nur Funktionstest, Messwerte nicht aussagekräftig\n"
        << "  --interval <s>              Berichtsabstand (Vorgabe 1)\n"
        << "  --duration <s>              Messdauer, 0 = bis Strg+C\n"
        << "  --output <datei|->          JSON Lines an Datei anhängen, - = keine (Vorgabe stdout)\n"
        << "  --http <adresse:port>       Web-Oberfläche (Vorgabe 127.0.0.1:8090); --no-http aus\n"
        << "  --webui <ordner>            Ordner der Web-Oberfläche\n"
        << "  --lcores <liste>            CPU-Kerne für MTL, z. B. 4-7\n"
        << "  --log-level <stufe>         debug, info, notice, warning, error\n"
        << "  -h, --help                  diese Hilfe\n";
    return out.str();
}

} // namespace st2110
