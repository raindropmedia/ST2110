#include "web/WebServer.h"

#include <chrono>
#include <cstdio>
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
    // Ohne TCP_NODELAY wartet der zweite Teil jeder Antwort auf das verzögerte ACK des Browsers (~40 ms,
    // Nagle-Algorithmus); das begrenzte die Vorschau auf rund 23 Bilder/s.
    s.set_tcp_nodelay(true);
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
    s.Get("/api/preview/video", [this](const httplib::Request& req, httplib::Response& res) {
        auto flow = hub_.flow(req.get_param_value("flow"));
        if (!flow || !flow->config().video) {
            sendJson(res, json{{"error", "Kein Videoflow mit dieser ID"}}, 404);
            return;
        }
        PreviewSettings settings;
        try {
            if (req.has_param("width")) {
                settings.maxWidth = static_cast<uint32_t>(std::stoul(req.get_param_value("width")));
            }
            if (req.has_param("fps")) {
                settings.fps = std::stod(req.get_param_value("fps"));
            }
            if (req.has_param("quality")) {
                settings.quality = std::stoi(req.get_param_value("quality"));
            }
        } catch (const std::exception&) {
            sendJson(res, json{{"error", "width, fps und quality müssen Zahlen sein"}}, 400);
            return;
        }
        uint64_t after = 0;
        if (req.has_param("after")) {
            try {
                after = std::stoull(req.get_param_value("after"));
            } catch (const std::exception&) {
                after = 0;
            }
        }
        flow->videoPreview().request(flow->clockNow(), settings);
        // Long Polling: auf ein neueres Bild warten (höchstens 250 ms)
        const PreviewImage image = flow->videoPreview().waitNewer(after, 0.25);
        const double now = flow->clockNow();
        res.set_header("Cache-Control", "no-store");
        if (image.jpeg.empty()) {
            res.status = 204; // erstes Bild folgt
            return;
        }
        res.set_header("X-Preview-Width", std::to_string(image.width));
        res.set_header("X-Preview-Height", std::to_string(image.height));
        res.set_header("X-Preview-Sequence", std::to_string(image.sequence));
        res.set_header("X-Preview-Age-Ms",
                       std::to_string(static_cast<int64_t>((now - image.timeSec) * 1000)));
        res.set_content(image.jpeg, "image/jpeg");
    });
    s.Get("/api/preview/audio", [this](const httplib::Request& req, httplib::Response& res) {
        auto flow = hub_.flow(req.get_param_value("flow"));
        if (!flow || !flow->config().audio) {
            sendJson(res, json{{"error", "Kein Tonflow mit dieser ID"}}, 404);
            return;
        }
        uint64_t from = 0;
        unsigned pair = 0;
        try {
            if (req.has_param("from")) {
                from = std::stoull(req.get_param_value("from"));
            }
            if (req.has_param("pair")) {
                pair = static_cast<unsigned>(std::stoul(req.get_param_value("pair")));
            }
        } catch (const std::exception&) {
            sendJson(res, json{{"error", "from und pair müssen Zahlen sein"}}, 400);
            return;
        }
        auto& preview = flow->audioPreview();
        preview.request(flow->clockNow());
        const auto chunk = preview.read(from, pair, flow->config().audio->format.sampleRate);
        std::string peaks;
        for (size_t i = 0; i < chunk.peaksDbfs.size(); ++i) {
            char buffer[16];
            std::snprintf(buffer, sizeof(buffer), "%s%.1f", i == 0 ? "" : ",", chunk.peaksDbfs[i]);
            peaks += buffer;
        }
        res.set_header("Cache-Control", "no-store");
        res.set_header("X-Audio-Start", std::to_string(chunk.start));
        res.set_header("X-Audio-Frames", std::to_string(chunk.frames));
        res.set_header("X-Audio-Rate", std::to_string(chunk.sampleRate));
        res.set_header("X-Audio-Channels", std::to_string(chunk.channels));
        res.set_header("X-Audio-Peaks", peaks);
        // 16 Bit, verschachtelt, Little Endian (Byte-Reihenfolge der Zielplattform x86/ARM)
        const auto* bytes = reinterpret_cast<const char*>(chunk.stereo.data());
        res.set_content(std::string(bytes, bytes + chunk.stereo.size() * sizeof(int16_t)),
                        "application/octet-stream");
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
