#pragma once

#include "core/FlowController.h"
#include "core/MonitorHub.h"
#include "core/TestSender.h"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace httplib {
class Server;
}

namespace st2110 {

// Web-Oberfläche: liefert die statischen Dateien aus webuiDir und eine JSON-API.
//   GET    /api/info                     Programmstand, Fähigkeiten
//   GET    /api/flows                    überwachte, erkannte und angekündigte Flows
//   POST   /api/flows                    Flow überwachen: {"kind": "video"|"audio", "spec": "ip:port,…"},
//                                        {"sdp": "v=0…"} oder {"sap": "<schlüssel>"}
//   DELETE /api/flows/<id>               Flow nicht mehr überwachen
//   GET    /api/snapshot?flow=v1&window=s Werte eines Flows, PTP, Ereignisse, Flow-Liste
//   GET    /api/preview/video?flow=v1&width=960&fps=25&quality=80&after=n
//                                        Vorschaubild als JPEG; wartet bis 250 ms auf ein Bild neuer als n.
//                                        Kopfzeilen X-Preview-Width/-Height/-Sequence/-Age-Ms, 204 bis zum
//                                        ersten Bild. Ein Abruf hält die Vorschau 3 s lang aktiv.
//   GET    /api/preview/audio?flow=a1&from=n&pair=0  Ton ab Abtastwert n (16 Bit stereo LE, Kopfzeilen
//                                        X-Audio-Start/-Frames/-Rate/-Channels/-Peaks)
//   GET    /api/tx                       Testsender: Sendeport, Höchstzahl, Einträge mit Zustand
//                                        (stopped, starting, sending, stopping, error); {"available": false},
//                                        wenn kein Sendeport konfiguriert ist (--tx-port)
//   POST   /api/tx                       Testsender anlegen: {"kind": "video"|"audio", "spec": "ip:port,…",
//                                        "start": true|false}; spec wie bei --tx-video bzw. --tx-audio
//   PUT    /api/tx/<id>                  Einstellungen ändern ({"spec": "…"}), nur im Zustand stopped/error
//   POST   /api/tx/<id>/start            senden (der Start läuft im Hintergrund: MTL wartet auf PTP)
//   POST   /api/tx/<id>/stop             Senden beenden
//   GET    /api/tx/<id>/sdp              SDP-Beschreibung des Testsenders (application/sdp)
//   DELETE /api/tx/<id>                  Testsender löschen
//   GET    /api/system                   Host (Kernel, CPU, Hugepages, IOMMU) und Netzwerkkarten: PCI,
//   Treiber,
//                                        Link, ethtool-Angaben und SFP-Modul (Hersteller, Seriennummer,
//                                        Diagnose- werte); Ports an vfio-pci zeigen die Momentaufnahme vom
//                                        Binden (live: false). Nur lesend; 5 s zwischengespeichert.
//   POST   /api/reset                    Messwerte zurücksetzen
//   GET    /api/demo                     Störungen der Demo (nur mit --demo)
//   POST   /api/demo                     {"preset": "burst"}
// Ohne Anmeldung: nur im Messnetz bzw. auf 127.0.0.1 betreiben.
class WebServer {
  public:
    struct DemoControl {
        std::function<std::vector<std::pair<std::string, std::string>>()> presets; // id, Bezeichnung
        std::function<std::string()> current;
        std::function<bool(const std::string&)> set;
    };

    // testSender darf nullptr sein (kein Sendeport).
    WebServer(MonitorHub& hub, FlowController* controller, TestSender* testSender, nlohmann::json info,
              std::string webuiDir, std::optional<DemoControl> demo);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Startet den Server im Hintergrund; false, wenn Adresse oder Ordner nicht nutzbar sind.
    bool start(const std::string& host, uint16_t port, std::string& error);
    void stop();

    // Quelle für GET /api/system; ohne sie antwortet der Endpunkt mit {"available": false}.
    void setSystemInfo(std::function<nlohmann::json()> provider);

  private:
    MonitorHub& hub_;
    FlowController* controller_;
    TestSender* testSender_;
    nlohmann::json info_;
    std::string webuiDir_;
    std::optional<DemoControl> demo_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
    std::function<nlohmann::json()> systemInfo_;
    std::mutex systemMutex_;
    nlohmann::json systemCache_;
    std::chrono::steady_clock::time_point systemCacheTime_{};
};

} // namespace st2110
