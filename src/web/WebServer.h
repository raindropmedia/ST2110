#pragma once

#include "core/FlowController.h"
#include "core/MonitorHub.h"

#include <functional>
#include <memory>
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
//   GET    /api/preview/video?flow=v1    Vorschaubild (RGB, Kopfzeilen
//   X-Preview-Width/-Height/-Sequence/-Age-Ms;
//                                        204 bis zum ersten Bild); Abruf hält die Vorschau 3 s lang aktiv
//   GET    /api/preview/audio?flow=a1&from=n&pair=0  Ton ab Abtastwert n (16 Bit stereo LE, Kopfzeilen
//                                        X-Audio-Start/-Frames/-Rate/-Channels/-Peaks)
//   POST   /api/reset                    Messwerte zurücksetzen
//   GET  /api/demo              Störungen der Demo (nur mit --demo)
//   POST /api/demo              {"preset": "burst"}
// Ohne Anmeldung: nur im Messnetz bzw. auf 127.0.0.1 betreiben.
class WebServer {
  public:
    struct DemoControl {
        std::function<std::vector<std::pair<std::string, std::string>>()> presets; // id, Bezeichnung
        std::function<std::string()> current;
        std::function<bool(const std::string&)> set;
    };

    WebServer(MonitorHub& hub, FlowController* controller, nlohmann::json info, std::string webuiDir,
              std::optional<DemoControl> demo);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Startet den Server im Hintergrund; false, wenn Adresse oder Ordner nicht nutzbar sind.
    bool start(const std::string& host, uint16_t port, std::string& error);
    void stop();

  private:
    MonitorHub& hub_;
    FlowController* controller_;
    nlohmann::json info_;
    std::string webuiDir_;
    std::optional<DemoControl> demo_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};

} // namespace st2110
