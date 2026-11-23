// st2110-monitor: Messgerät für SMPTE ST 2110 auf Basis von MTL (siehe README.md).

#include "core/MonitorHub.h"
#include "core/NetworkInfo.h"
#include "core/Options.h"
#include "demo/DemoSource.h"
#include "demo/DemoTxBackend.h"
#include "discovery/Discovery.h"
#include "web/WebServer.h"

#ifdef ST2110_WITH_MTL
#include "mtl/MtlSource.h"
#endif

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#ifndef ST2110_WEBUI_DIR
#define ST2110_WEBUI_DIR "webui"
#endif
#ifndef ST2110_VERSION
#define ST2110_VERSION "0.0.0"
#endif

namespace {

std::atomic<bool> gStop{false};

extern "C" void onSignal(int /*signal*/) {
    gStop = true;
}

void installSignalHandlers() {
    struct sigaction action {};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);
}

bool hasPacketHook() {
#ifdef ST2110_WITH_MTL
    return st2110::MtlSource::hasPacketHook();
#else
    return false;
#endif
}

} // namespace

int main(int argc, char** argv) {
    using namespace st2110;
    using Clock = std::chrono::steady_clock;

    const std::vector<std::string> args(argv, argv + argc);
    const std::string program = args.empty() ? "st2110-monitor" : args.front();
    const ParseResult parsed = parseOptions(args);
    if (parsed.helpRequested) {
        std::cout << usageText(program);
        return 0;
    }
    for (const auto& warning : parsed.warnings) {
        std::cerr << "Hinweis: " << warning << '\n';
    }
    if (!parsed.options) {
        for (const auto& error : parsed.errors) {
            std::cerr << "Fehler: " << error << '\n';
        }
        std::cerr << "Hilfe: " << program << " --help\n";
        return 1;
    }
    const Options& options = *parsed.options;

    // Ohne Port und Flows: nur Erkennung (Netz-Scanner), MTL wird nicht gestartet.
    const bool scannerOnly = !options.demo && options.primaryPort.port.empty();
#ifndef ST2110_WITH_MTL
    if (!options.demo && !scannerOnly) {
        std::cerr << "Fehler: Ohne MTL gebaut – nur --demo oder reine Erkennung (--discover) möglich.\n";
        return 2;
    }
#endif

    std::ofstream file;
    const bool writeRecords = options.outputPath != "-";
    if (writeRecords && !options.outputPath.empty()) {
        file.open(options.outputPath, std::ios::app);
        if (!file) {
            std::cerr << "Fehler: Ausgabedatei kann nicht geöffnet werden: " << options.outputPath << '\n';
            return 1;
        }
    }
    std::ostream& out = options.outputPath.empty() ? std::cout : file;
    auto writeLine = [&](const nlohmann::json& record) {
        if (writeRecords) {
            out << record.dump() << '\n' << std::flush;
        }
    };

    installSignalHandlers();

    // Erkennung: Ohne --discover liest das Programm im DPDK-Betrieb auf dem Verwaltungsport mit
    // (Schnittstelle mit der Standardroute). Die Karte selbst sieht nur abonnierte Flows.
    std::string discoverInterface = options.discoverInterface;
    if (discoverInterface.empty() && options.autoDiscover && !options.demo && !scannerOnly) {
        discoverInterface = defaultRouteInterface();
        if (discoverInterface.empty()) {
            std::cerr
                << "Hinweis: Keine Standardroute gefunden, Erkennung aus (mit --discover <schnittstelle> "
                   "einschalten).\n";
        } else {
            std::cerr << "Erkennung auf dem Verwaltungsport " << discoverInterface
                      << " (automatisch; --no-discover schaltet sie ab).\n";
        }
    }

    HubConfig config;
    config.hwTimestamp = options.hwTimestamp;
    config.ptp = options.ptp;
    config.demo = options.demo;
    config.maxFlows = options.maxFlows;
    config.discoverInterface = discoverInterface;
    MonitorHub hub(config);

    std::unique_ptr<DemoSource> demo;
    FlowController* controller = nullptr;
#ifdef ST2110_WITH_MTL
    std::unique_ptr<MtlSource> mtl;
#endif
    // Testsender: bei MTL gehört die Verwaltung der Quelle, in der Demo gibt es eine ohne Senden.
    TestSender* testSender = nullptr;
    std::unique_ptr<DemoTxBackend> demoTxBackend;
    std::unique_ptr<TestSender> demoTestSender;
    if (options.demo) {
        demo = std::make_unique<DemoSource>(options, hub);
        controller = demo.get();
        demoTxBackend = std::make_unique<DemoTxBackend>(hub);
        demoTestSender = std::make_unique<TestSender>(*demoTxBackend, options.maxTxFlows, "demo", "");
        testSender = demoTestSender.get();
    } else if (!scannerOnly) {
#ifdef ST2110_WITH_MTL
        std::string error;
        mtl = MtlSource::create(options, hub, error);
        if (!mtl) {
            std::cerr << "Fehler: " << error << '\n';
            return 2;
        }
        controller = mtl.get();
        testSender = mtl->testSender();
#endif
    }

    std::unique_ptr<Discovery> discovery;
    if (!discoverInterface.empty()) {
        discovery = std::make_unique<Discovery>(hub, discoverInterface, options.sap);
        std::string error;
        if (!discovery->start(error)) {
            std::cerr << "Hinweis: Erkennung nicht verfügbar: " << error << '\n';
        }
    }

    std::unique_ptr<WebServer> web;
    if (!options.httpListen.empty()) {
        std::optional<WebServer::DemoControl> control;
        if (demo) {
            DemoSource* d = demo.get();
            control = WebServer::DemoControl{
                [] {
                    std::vector<std::pair<std::string, std::string>> list;
                    for (const auto& p : DemoSource::presets()) {
                        list.emplace_back(p.id, p.label);
                    }
                    return list;
                },
                [d] { return d->preset(); },
                [d](const std::string& id) { return d->setPreset(id); },
            };
        }
        nlohmann::json info{{"program", "st2110-monitor"},
                            {"version", ST2110_VERSION},
                            {"packetHook", options.demo || hasPacketHook()},
                            {"hwTimestamp", options.hwTimestamp},
                            {"ptp", options.ptp}};
        web = std::make_unique<WebServer>(hub, controller, testSender, info,
                                          options.webuiDir.empty() ? ST2110_WEBUI_DIR : options.webuiDir,
                                          control);
        std::string host;
        uint16_t port = 0;
        parseListen(options.httpListen, host, port);
        std::string error;
        if (!web->start(host, port, error)) {
            std::cerr << "Fehler: " << error << '\n';
            return 2;
        }
        std::cerr << "Web-Oberfläche: http://" << (host == "0.0.0.0" ? "<adresse>" : host) << ':' << port
                  << "/\n";
    }

    writeLine(hub.startRecord());

    const auto start = Clock::now();
    auto nextReport = start + std::chrono::seconds(options.intervalSec);
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        const bool done = options.durationSec > 0 && now - start >= std::chrono::seconds(options.durationSec);
        if (now >= nextReport || done) {
            nextReport += std::chrono::seconds(options.intervalSec);
            writeLine(hub.takeIntervalRecord());
        }
        if (done) {
            break;
        }
    }

    web.reset();
    discovery.reset();
    demoTestSender.reset();
    demoTxBackend.reset();
    demo.reset();
#ifdef ST2110_WITH_MTL
    mtl.reset();
#endif
    writeLine(hub.summaryRecord());

    if (!hub.flows().empty() && hub.received() == 0) {
        std::cerr << "Fehler: Nichts empfangen (Adresse, Port, IGMP, Format prüfen).\n";
        return 3;
    }
    return 0;
}
