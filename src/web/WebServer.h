#pragma once

#include "core/Monitor.h"

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
//   GET  /api/info              Programmstand, Fähigkeiten
//   GET  /api/snapshot?window=s Messwerte, Histogramme, Verläufe, Ereignisse
//   POST /api/reset             Messwerte zurücksetzen
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

    WebServer(Monitor& monitor, nlohmann::json info, std::string webuiDir, std::optional<DemoControl> demo);
    ~WebServer();
    WebServer(const WebServer&) = delete;
    WebServer& operator=(const WebServer&) = delete;

    // Startet den Server im Hintergrund; false, wenn Adresse oder Ordner nicht nutzbar sind.
    bool start(const std::string& host, uint16_t port, std::string& error);
    void stop();

  private:
    Monitor& monitor_;
    nlohmann::json info_;
    std::string webuiDir_;
    std::optional<DemoControl> demo_;
    std::unique_ptr<httplib::Server> server_;
    std::thread thread_;
};

} // namespace st2110
