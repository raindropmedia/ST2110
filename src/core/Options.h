#pragma once

#include "core/AudioFormat.h"
#include "core/FrameRate.h"
#include "core/Sdp.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st2110 {

// Ein Port der Karte (ST 2022-7: primär und redundant).
struct PortOptions {
    // DPDK: PCI-Adresse der PF oder VF ("0000:af:00.0"); ohne DPDK "kernel:<Schnittstelle>".
    std::string port;
    // Eigene IPv4-Adresse am Port (bei DPDK Pflicht, MTL beantwortet damit ARP und IGMP).
    std::string localIp;
};

// Adresse eines Stroms auf einem Pfad.
struct FlowAddress {
    std::string ip; // Multicast-Gruppe oder Unicast-Adresse
    uint16_t udpPort = 0;
    std::string sourceIp; // Source-Specific Multicast (optional)
};

struct VideoFlow {
    FlowAddress primary;
    std::optional<FlowAddress> redundant;
    VideoFormat format;
    uint8_t payloadType = 96;
};

struct AudioFlow {
    FlowAddress primary;
    std::optional<FlowAddress> redundant;
    AudioFormat format;
    uint8_t payloadType = 97;
};

enum class Media { Video, Audio };

// Ein überwachter Flow: genau eines von video/audio ist gesetzt.
struct FlowConfig {
    std::string id;    // kurz und eindeutig, z. B. "v1", "a2" (leer: vergibt der Hub)
    std::string label; // Anzeige, z. B. "Kamera 1"
    std::optional<VideoFlow> video;
    std::optional<AudioFlow> audio;

    Media media() const { return video ? Media::Video : Media::Audio; }
    std::string formatName() const;
    const FlowAddress& primary() const { return video ? video->primary : audio->primary; }
    const std::optional<FlowAddress>& redundant() const {
        return video ? video->redundant : audio->redundant;
    }
    uint8_t payloadType() const { return video ? video->payloadType : audio->payloadType; }
};

// Beschreibung eines Flows wie auf der Kommandozeile: "ip:port[,schlüssel=wert…]".
// Video: red, format, pt, source, red-source, name. Ton: red, format (L24/48000/2), ptime, pt, source,
// red-source, name. Liefert false und eine Fehlermeldung bei ungültigen Angaben.
bool parseFlowSpec(Media media, const std::string& text, FlowConfig& flow, std::string& error);
// Alle Video- und Tonmedien einer SDP-Beschreibung (DUP-Gruppen ergeben P und R).
std::vector<FlowConfig> flowsFromSdp(const std::string& sdpText, std::vector<std::string>& errors);
// Prüft einen Flow (Adressen, Format, Payload-Typ); Meldungen mit dem Namen des Flows.
void validateFlow(const FlowConfig& flow, std::vector<std::string>& errors,
                  std::vector<std::string>& warnings);

enum class LogLevel { Debug, Info, Notice, Warning, Error };

// Optionen des Messprogramms st2110-monitor.
struct Options {
    // Synthetische Ströme ohne Karte (Vorführung, Lehre, Test der Oberfläche).
    bool demo = false;

    PortOptions primaryPort;
    std::optional<PortOptions> redundantPort;
    std::vector<FlowConfig> flows;
    // Höchstzahl gleichzeitig überwachter Flows (MTL reserviert dafür Empfangs-Queues).
    unsigned maxFlows = 8;

    // Flow-Erkennung: Kernel-Schnittstelle, deren Pakete mitgelesen werden (leer = aus), und
    // SAP-Ankündigungen auf derselben Schnittstelle.
    std::string discoverInterface;
    bool sap = true;

    // Hardware-Zeitstempel der Karte für jedes empfangene Paket (MTL_FLAG_ENABLE_HW_TIMESTAMP).
    bool hwTimestamp = true;
    // Eingebautes PTP von MTL (nur auf der PF; nutzt die Uhr von Port 0 der Karte).
    bool ptp = true;

    // Abstand der Berichte in Sekunden und Messdauer (0 = bis Strg+C).
    unsigned intervalSec = 1;
    unsigned durationSec = 0;

    std::string lcores;
    LogLevel logLevel = LogLevel::Info;

    // Berichte als JSON Lines: leer = stdout, "-" = keine, sonst Datei (wird angehängt).
    std::string outputPath;

    // Web-Oberfläche: "adresse:port", leer = aus.
    std::string httpListen = "127.0.0.1:8090";
    // Ordner mit index.html (Vorgabe: beim Bauen festgelegt).
    std::string webuiDir;
};

struct ParseResult {
    std::optional<Options> options;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    bool helpRequested = false;
};

// Liest die Kommandozeile (argv[0] wird übersprungen) und prüft die Werte. readFile liest SDP-Dateien
// (für Tests austauschbar); liefert false, wenn die Datei fehlt.
using FileReader = bool (*)(const std::string& path, std::string& content);
bool readTextFile(const std::string& path, std::string& content);
ParseResult parseOptions(const std::vector<std::string>& args, FileReader readFile = readTextFile);

void validateOptions(const Options& options, std::vector<std::string>& errors,
                     std::vector<std::string>& warnings);

std::string usageText(const std::string& programName);

bool isPciAddress(const std::string& text);
bool isKernelPort(const std::string& text);
bool isIpv4Address(const std::string& text);
bool isMulticastIpv4(const std::string& text);
// "239.1.1.1:20000"
std::optional<FlowAddress> parseFlowAddress(const std::string& text);
// "127.0.0.1:8090" -> Host und Port
bool parseListen(const std::string& text, std::string& host, uint16_t& port);

} // namespace st2110
