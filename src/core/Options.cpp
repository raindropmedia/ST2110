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

void validateFlow(const FlowAddress& flow, const std::string& label, std::vector<std::string>& errors,
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

void validateOptions(const Options& o, std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    if (!o.video && !o.audio) {
        errors.push_back("Kein Strom angegeben (--video, --audio oder --sdp).");
    }
    const bool anyRedundant = (o.video && o.video->redundant) || (o.audio && o.audio->redundant);
    if (!o.demo) {
        validatePort(o.primaryPort, "Port P", o, errors);
        if (o.redundantPort) {
            validatePort(*o.redundantPort, "Port R", o, errors);
            if (o.redundantPort->port == o.primaryPort.port) {
                errors.push_back("Port P und Port R sind derselbe Port.");
            }
        }
        if (anyRedundant && !o.redundantPort) {
            errors.push_back("Redundanter Strom ohne redundanten Port (--red-port, --red-local-ip).");
        }
        if (o.redundantPort && !anyRedundant) {
            warnings.push_back("Port R ist angegeben, aber kein Strom nutzt ihn.");
        }
    }
    if (o.video) {
        validateFlow(o.video->primary, "Video P", errors, warnings);
        if (o.video->redundant) {
            validateFlow(*o.video->redundant, "Video R", errors, warnings);
        }
        const auto& f = o.video->format;
        if (f.width < 64 || f.width > 16384 || f.width % 2 != 0) {
            errors.push_back("Video: Breite muss gerade sein und zwischen 64 und 16384 liegen.");
        }
        if (f.height < 64 || f.height > 8704 || (f.interlaced && f.height % 2 != 0)) {
            errors.push_back("Video: Höhe muss zwischen 64 und 8704 liegen (bei Zeilensprung gerade).");
        }
        if (o.video->payloadType > 127) {
            errors.push_back("Video: Payload-Typ muss zwischen 0 und 127 liegen.");
        }
    }
    if (o.audio) {
        validateFlow(o.audio->primary, "Ton P", errors, warnings);
        if (o.audio->redundant) {
            validateFlow(*o.audio->redundant, "Ton R", errors, warnings);
        }
        if (auto problem = validateAudioFormat(o.audio->format); !problem.empty()) {
            errors.push_back("Ton: " + problem);
        }
        if (o.audio->payloadType > 127) {
            errors.push_back("Ton: Payload-Typ muss zwischen 0 und 127 liegen.");
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

ParseResult parseOptions(const std::vector<std::string>& args, FileReader readFile) {
    ParseResult result;
    Options o;
    VideoFlow video;
    AudioFlow audio;
    bool videoSet = false;
    bool audioSet = false;
    bool videoRedSet = false;
    bool audioRedSet = false;
    std::string videoSource, videoRedSource, audioSource, audioRedSource;
    PortOptions redPort;
    bool redPortSet = false;
    std::vector<std::string> sdpFiles;

    auto flowSetter = [](FlowAddress& target, bool& flag) {
        return [&target, &flag](const std::string& v) {
            auto flow = parseFlowAddress(v);
            if (flow) {
                target = *flow;
                flag = true;
            }
            return flow.has_value();
        };
    };
    FlowAddress videoRed, audioRed;

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
        {"--video", flowSetter(video.primary, videoSet)},
        {"--video-red", flowSetter(videoRed, videoRedSet)},
        {"--video-source",
         [&](const std::string& v) {
             videoSource = v;
             return true;
         }},
        {"--video-red-source",
         [&](const std::string& v) {
             videoRedSource = v;
             return true;
         }},
        {"--video-format",
         [&](const std::string& v) {
             auto f = parseVideoFormat(v);
             if (f) {
                 video.format = *f;
             }
             return f.has_value();
         }},
        {"--video-pt", [&](const std::string& v) { return parseUnsigned(v, video.payloadType); }},
        {"--audio", flowSetter(audio.primary, audioSet)},
        {"--audio-red", flowSetter(audioRed, audioRedSet)},
        {"--audio-source",
         [&](const std::string& v) {
             audioSource = v;
             return true;
         }},
        {"--audio-red-source",
         [&](const std::string& v) {
             audioRedSource = v;
             return true;
         }},
        {"--audio-format", [&](const std::string& v) { return parseAudioEncoding(v, audio.format); }},
        {"--audio-ptime",
         [&](const std::string& v) {
             auto p = parsePacketTime(v);
             if (p) {
                 audio.format.ptime = *p;
             }
             return p.has_value();
         }},
        {"--audio-pt", [&](const std::string& v) { return parseUnsigned(v, audio.payloadType); }},
        {"--sdp",
         [&](const std::string& v) {
             sdpFiles.push_back(v);
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
        {"--demo", [&] { o.demo = true; }},
        {"--no-hw-timestamp", [&] { o.hwTimestamp = false; }},
        {"--no-ptp", [&] { o.ptp = false; }},
        {"--no-http", [&] { o.httpListen.clear(); }},
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
            if (name == "--video-format") {
                message +=
                    " (Form BREITExHÖHE{p|i}RATE, z. B. 1920x1080p50; Raten: " + frameRateNames() + ")";
            } else if (name == "--audio-format") {
                message += " (Form L24/48000/2)";
            } else if (name == "--audio-ptime") {
                message += " (0.125, 0.25, 0.333, 1 oder 4)";
            } else if (name == "--video" || name == "--audio" || name == "--video-red" ||
                       name == "--audio-red") {
                message += " (Form 239.1.1.1:20000)";
            }
            result.errors.push_back(message + ".");
        }
    }

    // SDP zuerst, Kommandozeile überschreibt Adressen nicht: SDP füllt nur fehlende Ströme.
    for (const auto& file : sdpFiles) {
        std::string text;
        if (!readFile(file, text)) {
            result.errors.push_back("SDP-Datei kann nicht gelesen werden: " + file);
            continue;
        }
        std::string error;
        auto session = parseSdp(text, error);
        if (!session) {
            result.errors.push_back(file + ": " + error);
            continue;
        }
        if (!videoSet) {
            std::string verror;
            if (auto v = sdpVideo(*session, verror)) {
                video.primary = fromSdp(v->primary);
                video.format = v->format;
                video.payloadType = static_cast<uint8_t>(v->payloadType);
                videoSet = true;
                if (v->redundant && !videoRedSet) {
                    videoRed = fromSdp(*v->redundant);
                    videoRedSet = true;
                }
            } else if (!verror.empty()) {
                result.errors.push_back(file + ": " + verror);
            }
        }
        if (!audioSet) {
            std::string aerror;
            if (auto a = sdpAudio(*session, aerror)) {
                audio.primary = fromSdp(a->primary);
                audio.format = a->format;
                audio.payloadType = static_cast<uint8_t>(a->payloadType);
                audioSet = true;
                if (a->redundant && !audioRedSet) {
                    audioRed = fromSdp(*a->redundant);
                    audioRedSet = true;
                }
            } else if (!aerror.empty()) {
                result.errors.push_back(file + ": " + aerror);
            }
        }
    }

    if (o.demo && !videoSet && !audioSet) {
        video.primary = FlowAddress{"239.0.0.1", 20000, {}};
        videoRed = FlowAddress{"239.0.1.1", 20000, {}};
        audio.primary = FlowAddress{"239.0.0.2", 30000, {}};
        audioRed = FlowAddress{"239.0.1.2", 30000, {}};
        videoSet = audioSet = videoRedSet = audioRedSet = true;
    }
    if (videoSet) {
        if (!videoSource.empty()) {
            video.primary.sourceIp = videoSource;
        }
        if (videoRedSet) {
            if (!videoRedSource.empty()) {
                videoRed.sourceIp = videoRedSource;
            }
            video.redundant = videoRed;
        }
        o.video = video;
    } else if (videoRedSet) {
        result.errors.push_back("--video-red ohne --video.");
    }
    if (audioSet) {
        if (!audioSource.empty()) {
            audio.primary.sourceIp = audioSource;
        }
        if (audioRedSet) {
            if (!audioRedSource.empty()) {
                audioRed.sourceIp = audioRedSource;
            }
            audio.redundant = audioRed;
        }
        o.audio = audio;
    } else if (audioRedSet) {
        result.errors.push_back("--audio-red ohne --audio.");
    }
    if (redPortSet) {
        o.redundantPort = redPort;
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

std::string usageText(const std::string& programName) {
    std::ostringstream out;
    out << "Aufruf: " << programName << " [Optionen]\n"
        << "\n"
        << "Messgerät für SMPTE ST 2110 auf Basis von MTL: Video (ST 2110-20/-21), Ton (ST 2110-30),\n"
        << "PTP und ST 2022-7. Web-Oberfläche mit Histogrammen und Verläufen, Berichte als JSON Lines.\n"
        << "\n"
        << "Ports der Karte:\n"
        << "  --port <pci|kernel:if>      Port P, z. B. 0000:af:00.0 (PF von Port 0 für PTP)\n"
        << "  --local-ip <ipv4>           eigene Adresse an Port P\n"
        << "  --red-port, --red-local-ip  Port R für ST 2022-7\n"
        << "\n"
        << "Ströme (Adresse als ip:port):\n"
        << "  --sdp <datei>               SDP lesen (mehrfach möglich; DUP-Gruppen ergeben P und R)\n"
        << "  --video <ip:port>           Video P;  --video-red <ip:port> Video R\n"
        << "  --video-format <fmt>        z. B. 1920x1080p50, 1920x1080i50 (Halbbilder: i50 = 25 Bilder)\n"
        << "  --video-pt <n>              RTP-Payload-Typ (Vorgabe 96)\n"
        << "  --video-source, --video-red-source <ipv4>  Quelle für SSM\n"
        << "  --audio <ip:port>           Ton P;  --audio-red <ip:port> Ton R\n"
        << "  --audio-format <L24/48000/2>  Kodierung/Abtastrate/Kanäle (Vorgabe L24/48000/2)\n"
        << "  --audio-ptime <ms>          0.125, 0.25, 0.333, 1, 4 (Vorgabe 1)\n"
        << "  --audio-pt <n>              RTP-Payload-Typ (Vorgabe 97)\n"
        << "  --audio-source, --audio-red-source <ipv4>\n"
        << "\n"
        << "Messung und Ausgabe:\n"
        << "  --demo                      synthetische Ströme ohne Karte (Vorführung, Test)\n"
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
