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
    // Packmodus des Senders (SDP PM=2110BPM/2110GPM); MTL muss ihn beim Empfang kennen.
    Packing packing = Packing::Bpm;
    // Nur Testsender: Vorlauf des ersten Pakets in Paketzeiten vor TR_OFFSET (MTL start_vrx), 0 = MTL wählt
    // selbst (bei Ratenbegrenzung narrow − 4). Schlüssel start-vrx.
    uint16_t startVrx = 0;
};

struct AudioFlow {
    FlowAddress primary;
    std::optional<FlowAddress> redundant;
    AudioFormat format;
    uint8_t payloadType = 97;
};

// Zusatzdaten nach ST 2110-40 (RFC 8331). Das Format (Bildrate, Zeilensprung) legt die Bildperiode fest, auf
// die sich die ANC-Pakete beziehen; Auflösung spielt keine Rolle.
struct AncFlow {
    FlowAddress primary;
    std::optional<FlowAddress> redundant;
    VideoFormat format;
    uint8_t payloadType = 100;
};

// SDI-Transport nach ST 2022-6 (High Bit Rate Media Transport). Das Format steht im Nutzdaten-Header.
struct HbrmtFlow {
    FlowAddress primary;
    std::optional<FlowAddress> redundant;
    uint8_t payloadType = 98; // ST 2022-6: 98 (27-MHz-Takt) bzw. 99 (FEC)
};

enum class Media { Video, Audio, Anc, Hbrmt };

// Ein überwachter Flow: genau eines von video/audio/anc/hbrmt ist gesetzt.
struct FlowConfig {
    std::string id;    // kurz und eindeutig, z. B. "v1", "a2", "d3", "h4" (leer: vergibt der Hub)
    std::string label; // Anzeige, z. B. "Kamera 1"
    std::optional<VideoFlow> video;
    std::optional<AudioFlow> audio;
    std::optional<AncFlow> anc;
    std::optional<HbrmtFlow> hbrmt;

    Media media() const {
        return video ? Media::Video : audio ? Media::Audio : anc ? Media::Anc : Media::Hbrmt;
    }
    // "video" | "audio" | "anc" | "hbrmt" (JSON-Schlüssel und Art in der Oberfläche)
    const char* kind() const;
    // Anzahl gesetzter Medien; gültig ist genau eines.
    int mediaCount() const { return (video ? 1 : 0) + (audio ? 1 : 0) + (anc ? 1 : 0) + (hbrmt ? 1 : 0); }
    std::string formatName() const;
    const FlowAddress& primary() const {
        return video ? video->primary : audio ? audio->primary : anc ? anc->primary : hbrmt->primary;
    }
    const std::optional<FlowAddress>& redundant() const {
        return video ? video->redundant : audio ? audio->redundant : anc ? anc->redundant : hbrmt->redundant;
    }
    uint8_t payloadType() const {
        return video   ? video->payloadType
               : audio ? audio->payloadType
               : anc   ? anc->payloadType
                       : hbrmt->payloadType;
    }
};

// Beschreibung eines Flows wie auf der Kommandozeile: "ip:port[,schlüssel=wert…]".
// Video: red, format, sampling (YCbCr-4:2:2 …, RGB), depth (8, 10, 12, 16), packing (BPM, GPM, GPM_SL), pt,
// source, red-source, name. Ton: red, format (L24/48000/2), ptime, pt, source,
// red-source, name. Zusatzdaten (ST 2110-40): red, format (nur Bildrate und Zeilensprung zählen, z. B.
// 1920x1080p50), pt, source, red-source, name. ST 2022-6: red, pt, source, red-source, name. Liefert false
// und eine Fehlermeldung bei ungültigen Angaben.
bool parseFlowSpec(Media media, const std::string& text, FlowConfig& flow, std::string& error);
// Alle Video- und Tonmedien einer SDP-Beschreibung (DUP-Gruppen ergeben P und R).
std::vector<FlowConfig> flowsFromSdp(const std::string& sdpText, std::vector<std::string>& errors);
// Prüft einen Flow (Adressen, Format, Payload-Typ); Meldungen mit dem Namen des Flows.
void validateFlow(const FlowConfig& flow, std::vector<std::string>& errors,
                  std::vector<std::string>& warnings);

// Mehr Sende-Queues als hier hat die E830 (E830-L, Firmware 1.3833, ice/DPDK 26.07) am Sendeport nicht
// gestartet: "ice_tx_queue_start(): Failed to add lan txq", Queue 8 (die neunte); acht Queues liefen. Das
// Gerät startet dann nicht, und mtl_init scheitert. Mit --tx-queues lässt sich mehr versuchen.
constexpr unsigned kAutoTxQueueLimit = 7;

// Sende-Queues, die MTL für den Sendeport reservieren soll: ein Videosender braucht eine, Ton mit Paketzeit
// unter 0,5 ms zwei (Ratenbegrenzung), dazu eine gemeinsame Tonqueue.
unsigned txQueueCount(unsigned maxTxFlows, unsigned override);

// Pacing der Sendeports (MTL mtl_init_params.pacing). Auto wählt auf der E8xx die Ratenbegrenzung, TSN
// (Launch-Time der Karte) nur auf ausdrücklichen Wunsch; es braucht die PF, PTP und
// Launch-Time-Unterstützung.
enum class TxPacing { Auto, RateLimit, Tsc, Tsn };

std::optional<TxPacing> parseTxPacing(const std::string& text);
const char* txPacingName(TxPacing pacing);

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

    // Testsender (nur mit MTL): sendet Testbild bzw. Testton über einen eigenen Port derselben
    // MTL-Instanz; Zeitbezug wie beim Empfang (PTP über MTL). Ohne ST 2022-7.
    // Weitere Testsender lassen sich zur Laufzeit in der Oberfläche anlegen (höchstens maxTxFlows; MTL
    // reserviert dafür beim Start Sende-Queues).
    std::optional<PortOptions> txPort;
    std::vector<FlowConfig> txFlows;
    unsigned maxTxFlows = 4;
    // Sende-Queues des Sendeports; 0 = automatisch (2 je Sender, höchstens kAutoTxQueueLimit).
    unsigned txQueues = 0;
    TxPacing txPacing = TxPacing::Auto;

    // Flow-Erkennung: Kernel-Schnittstelle, deren Pakete mitgelesen werden (leer = aus), und
    // SAP-Ankündigungen auf derselben Schnittstelle.
    // Ohne Angabe liest das Programm im DPDK-Betrieb auf dem Verwaltungsport mit (Schnittstelle mit der
    // Standardroute); autoDiscover = false (--no-discover) schaltet das ab.
    std::string discoverInterface;
    bool autoDiscover = true;
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
