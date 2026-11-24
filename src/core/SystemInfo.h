#pragma once

#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace st2110 {

// Zugriff auf das System, austauschbar für Tests und die Demo.
class SystemProbe {
  public:
    struct CommandResult {
        bool started = false; // Programm gefunden und gestartet
        int status = -1;      // Exit-Code
        std::string output;   // Standardausgabe und Fehlerausgabe
    };

    virtual ~SystemProbe() = default;
    virtual std::optional<std::string> readFile(const std::string& path) = 0;
    virtual std::optional<std::string> readLink(const std::string& path) = 0;
    virtual std::vector<std::string> listDir(const std::string& path) = 0; // nur Namen, sortiert
    // Startet ein Programm ohne Shell, mit Zeitlimit; argv[0] wird im PATH gesucht.
    virtual CommandResult run(const std::vector<std::string>& argv) = 0;
};

// Echtes System (Linux: /sys, /proc, ethtool). Unter anderen Systemen liefert es nichts.
std::unique_ptr<SystemProbe> makeSystemProbe();

struct SystemPort {
    std::string pci;  // "0000:01:00.0"
    std::string role; // "Empfang", "Senden", …
};

struct SystemOptions {
    std::vector<SystemPort> ports;   // vom Programm benutzte Ports (sonst nur Erkennung nach Klasse)
    std::string managementInterface; // Schnittstelle mit der Standardroute
    // Ablage der Momentaufnahmen, die scripts/setup-host.sh vor dem Binden an vfio-pci schreibt.
    std::string snapshotDir = "/run/st2110/nic";
};

// --- Auswertung der Ausgaben (ohne Zugriff aufs System, einzeln testbar) ---

// "Schlüssel : Wert"-Zeilen (ethtool); Zeilen ohne Doppelpunkt werden Fortsetzungen des letzten Werts
// (ethtool schreibt Listen untereinander).
std::vector<std::pair<std::string, std::string>> parseKeyValueLines(const std::string& text);

// Ausgabe von "ethtool -m": {"present": bool, "message": "", "identity": [{key,value}],
// "dom": […], "alarms": […]}. Bei Fehler present=false und message mit der ersten Zeile.
nlohmann::json parseModuleInfo(const std::string& text, bool commandOk);

// Momentaufnahme-Datei: Abschnitte "### name" gefolgt vom Text; Schlüssel "time" ist die Unix-Zeit.
std::vector<std::pair<std::string, std::string>> parseSnapshotSections(const std::string& text);

// Alle Angaben für GET /api/system: Host, Netzwerkkarten (Eigenschaften, Link, SFP-Modul).
nlohmann::json collectSystemInfo(SystemProbe& probe, const SystemOptions& options);

} // namespace st2110
