#pragma once

#include "core/FrameRate.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace st2110 {

// Ein Empfangspfad (ST 2022-7: primär "P", redundant "R").
struct PathOptions {
    // DPDK: PCI-Adresse der PF oder VF ("0000:af:00.0"); ohne DPDK "kernel:<Schnittstelle>".
    std::string port;
    // Eigene IPv4-Adresse am Port (bei DPDK Pflicht, MTL beantwortet damit ARP und IGMP).
    std::string localIp;
    // Multicast-Gruppe oder Unicast-Adresse des Stroms.
    std::string streamIp;
    // Quelladresse für Source-Specific Multicast (optional).
    std::string sourceIp;
    uint16_t udpPort = 20000;
};

enum class LogLevel { Debug, Info, Notice, Warning, Error };

// Optionen des Messprogramms st20-timing.
struct Options {
    PathOptions primary;
    std::optional<PathOptions> redundant;

    uint8_t payloadType = 112;
    uint32_t width = 1920;
    uint32_t height = 1080;
    FrameRate rate = FrameRate::P50;
    bool interlaced = false;

    // Hardware-Zeitstempel der Karte für jedes empfangene Paket (MTL_FLAG_ENABLE_HW_TIMESTAMP).
    bool hwTimestamp = true;
    // Eingebautes PTP von MTL (nur auf der PF; nutzt die Uhr von Port 0 der Karte).
    bool ptp = true;

    // Abstand der Berichte in Sekunden und Messdauer (0 = bis Strg+C).
    unsigned intervalSec = 1;
    unsigned durationSec = 0;

    // CPU-Kerne für MTL (z. B. "4-7"), leer = Vorgabe von MTL.
    std::string lcores;
    LogLevel logLevel = LogLevel::Info;

    // Ausgabe der Berichte: leer = stdout, sonst Datei (JSON Lines, wird angehängt).
    std::string outputPath;
};

struct ParseResult {
    std::optional<Options> options;
    // Fehlermeldungen (deutsch); leer, wenn options gesetzt ist oder Hilfe verlangt wurde.
    std::vector<std::string> errors;
    // Hinweise, die den Start nicht verhindern (z. B. Messung ohne Hardware-Zeitstempel).
    std::vector<std::string> warnings;
    bool helpRequested = false;
};

// Liest die Kommandozeile (argv[0] wird übersprungen) und prüft die Werte.
ParseResult parseOptions(const std::vector<std::string>& args);

// Prüft eine bereits gefüllte Struktur; liefert Fehler und Hinweise.
void validateOptions(const Options& options, std::vector<std::string>& errors,
                     std::vector<std::string>& warnings);

std::string usageText(const std::string& programName);

bool isPciAddress(const std::string& text);
bool isKernelPort(const std::string& text);
bool isIpv4Address(const std::string& text);
bool isMulticastIpv4(const std::string& text);

} // namespace st2110
