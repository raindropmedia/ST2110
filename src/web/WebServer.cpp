#include "web/WebServer.h"

#include <chrono>
#include <filesystem>
#include <httplib.h>

namespace st2110 {

using nlohmann::json;

namespace {

void sendJson(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_header("Cache-Control", "no-store");
    res.set_content(body.dump(), "application/json; charset=utf-8");
}

} // namespace

WebServer::WebServer(MonitorHub& hub, FlowController* controller, json info, std::string webuiDir,
                     std::optional<DemoControl> demo)
    : hub_(hub), controller_(controller), info_(std::move(info)), webuiDir_(std::move(webuiDir)),
      demo_(std::move(demo)), server_(std::make_unique<httplib::Server>()) {}

WebServer::~WebServer() {
    stop();
}

bool WebServer::start(const std::string& host, uint16_t port, std::string& error) {
    if (!std::filesystem::exists(std::filesystem::path(webuiDir_) / "index.html")) {
        error = "Web-Oberfläche nicht gefunden: " + webuiDir_ + "/index.html (--webui angeben)";
        return false;
    }
    auto& s = *server_;
    if (!s.set_mount_point("/", webuiDir_)) {
        error = "Ordner der Web-Oberfläche nicht lesbar: " + webuiDir_;
        return false;
    }
    s.set_default_headers({{"X-Content-Type-Options", "nosniff"}, {"Referrer-Policy", "no-referrer"}});

    s.Get("/api/info", [this](const httplib::Request&, httplib::Response& res) {
        json info = info_;
        info["demo"] = demo_.has_value();
        sendJson(res, info);
    });
    s.Get("/api/snapshot", [this](const httplib::Request& req, httplib::Response& res) {
        double window = 60.0;
        if (req.has_param("window")) {
            try {
                window = std::clamp(std::stod(req.get_param_value("window")), 1.0, 240.0);
            } catch (const std::exception&) {
                sendJson(res, json{{"error", "window muss eine Zahl sein"}}, 400);
                return;
            }
        }
        sendJson(res,
                 hub_.snapshot(req.has_param("flow") ? req.get_param_value("flow") : std::string{}, window));
    });
    s.Post("/api/reset", [this](const httplib::Request&, httplib::Response& res) {
        hub_.reset();
        sendJson(res, json{{"ok", true}});
    });
    s.Get("/api/flows",
          [this](const httplib::Request&, httplib::Response& res) { sendJson(res, hub_.flowsJson()); });
    s.Post("/api/flows", [this](const httplib::Request& req, httplib::Response& res) {
        if (controller_ == nullptr) {
            sendJson(res, json{{"error", "Flows lassen sich hier nicht hinzufügen"}}, 409);
            return;
        }
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.is_object()) {
            sendJson(res, json{{"error", "JSON-Objekt erwartet"}}, 400);
            return;
        }
        std::vector<FlowConfig> flows;
        std::vector<std::string> errors;
        if (body.contains("spec") && body["spec"].is_string()) {
            const std::string kind = body.value("kind", "");
            if (kind != "video" && kind != "audio") {
                sendJson(res, json{{"error", "kind muss video oder audio sein"}}, 400);
                return;
            }
            FlowConfig flow;
            std::string error;
            if (!parseFlowSpec(kind == "video" ? Media::Video : Media::Audio, body["spec"].get<std::string>(),
                               flow, error)) {
                sendJson(res, json{{"error", error}}, 400);
                return;
            }
            flows.push_back(flow);
        } else if (body.contains("sdp") && body["sdp"].is_string()) {
            flows = flowsFromSdp(body["sdp"].get<std::string>(), errors);
        } else if (body.contains("sap") && body["sap"].is_string()) {
            const std::string sdp = hub_.announcedSdp(body["sap"].get<std::string>());
            if (sdp.empty()) {
                sendJson(res, json{{"error", "Ankündigung nicht (mehr) vorhanden"}}, 404);
                return;
            }
            flows = flowsFromSdp(sdp, errors);
        } else {
            sendJson(res, json{{"error", "Erwartet spec+kind, sdp oder sap"}}, 400);
            return;
        }
        json ids = json::array();
        for (const auto& flow : flows) {
            std::string error;
            const std::string id = controller_->addFlow(flow, error);
            if (id.empty()) {
                errors.push_back((flow.label.empty() ? flow.primary().ip : flow.label) + ": " + error);
            } else {
                ids.push_back(id);
            }
        }
        sendJson(res, json{{"ids", ids}, {"errors", errors}}, ids.empty() ? 400 : 200);
    });
    s.Delete(R"(/api/flows/([A-Za-z0-9_-]+))", [this](const httplib::Request& req, httplib::Response& res) {
        if (controller_ == nullptr) {
            sendJson(res, json{{"error", "Flows lassen sich hier nicht entfernen"}}, 409);
            return;
        }
        std::string error;
        if (!controller_->removeFlow(req.matches[1], error)) {
            sendJson(res, json{{"error", error}}, 404);
            return;
        }
        sendJson(res, json{{"ok", true}});
    });
    s.Get("/api/demo", [this](const httplib::Request&, httplib::Response& res) {
        if (!demo_) {
            sendJson(res, json{{"error", "Nur im Demo-Betrieb verfügbar"}}, 404);
            return;
        }
        json presets = json::array();
        for (const auto& [id, label] : demo_->presets()) {
            presets.push_back(json{{"id", id}, {"label", label}});
        }
        sendJson(res, json{{"current", demo_->current()}, {"presets", presets}});
    });
    s.Post("/api/demo", [this](const httplib::Request& req, httplib::Response& res) {
        if (!demo_) {
            sendJson(res, json{{"error", "Nur im Demo-Betrieb verfügbar"}}, 404);
            return;
        }
        const json body = json::parse(req.body, nullptr, false);
        if (body.is_discarded() || !body.contains("preset") || !body["preset"].is_string()) {
            sendJson(res, json{{"error", "Erwartet {\"preset\": \"…\"}"}}, 400);
            return;
        }
        if (!demo_->set(body["preset"].get<std::string>())) {
            sendJson(res, json{{"error", "Unbekannte Störung"}}, 400);
            return;
        }
        sendJson(res, json{{"ok", true}, {"current", demo_->current()}});
    });

    if (!s.bind_to_port(host, port)) {
        error = "Web-Oberfläche: " + host + ":" + std::to_string(port) + " ist belegt oder nicht verfügbar.";
        return false;
    }
    thread_ = std::thread([this] { server_->listen_after_bind(); });
    return true;
}

void WebServer::stop() {
    if (server_) {
        server_->stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

} // namespace st2110
