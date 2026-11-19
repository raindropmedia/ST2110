// st20-timing: Messprogramm für ST 2110-20/-21 auf Basis von MTL.
// Gibt je Intervall die Timing-Werte als JSON Lines aus (siehe README.md).

#include "core/Options.h"
#include "core/Report.h"
#include "mtl/St20TimingReceiver.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

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

void writeLine(std::ostream& out, const nlohmann::json& record) {
    out << record.dump() << '\n' << std::flush;
}

} // namespace

int main(int argc, char** argv) {
    using namespace st2110;
    using Clock = std::chrono::steady_clock;

    const std::vector<std::string> args(argv, argv + argc);
    const std::string program = args.empty() ? "st20-timing" : args.front();
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

    std::ofstream file;
    if (!options.outputPath.empty()) {
        file.open(options.outputPath, std::ios::app);
        if (!file) {
            std::cerr << "Fehler: Ausgabedatei kann nicht geöffnet werden: " << options.outputPath << '\n';
            return 1;
        }
    }
    std::ostream& out = options.outputPath.empty() ? std::cout : file;

    installSignalHandlers();

    std::string error;
    auto receiver = St20TimingReceiver::create(options, error);
    if (!receiver) {
        std::cerr << "Fehler: " << error << '\n';
        return 2;
    }

    writeLine(out, startRecord(options, std::nullopt, std::chrono::system_clock::now()));

    FrameCounters totalFrames;
    TimingStats totalPrimary;
    TimingStats totalRedundant;
    bool criteriaWritten = false;

    const auto start = Clock::now();
    auto nextReport = start + std::chrono::seconds(options.intervalSec);
    while (!gStop) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const auto now = Clock::now();
        const bool durationReached =
            options.durationSec > 0 && now - start >= std::chrono::seconds(options.durationSec);
        if (now < nextReport && !durationReached) {
            continue;
        }
        nextReport += std::chrono::seconds(options.intervalSec);

        if (!criteriaWritten) {
            if (auto criteria = receiver->passCriteria()) {
                writeLine(out, passCriteriaRecord(*criteria, std::chrono::system_clock::now()));
                criteriaWritten = true;
            }
        }

        auto interval = receiver->takeInterval();
        totalFrames.merge(interval.frames);
        totalPrimary.merge(interval.primary);
        if (interval.redundant) {
            totalRedundant.merge(*interval.redundant);
        }
        writeLine(out, intervalRecord("interval", options.intervalSec, interval.frames, interval.primary,
                                      interval.redundant ? &*interval.redundant : nullptr,
                                      std::chrono::system_clock::now()));
        if (durationReached) {
            break;
        }
    }

    receiver->stop();
    // Rest seit dem letzten Bericht in die Zusammenfassung übernehmen.
    auto rest = receiver->takeInterval();
    totalFrames.merge(rest.frames);
    totalPrimary.merge(rest.primary);
    if (rest.redundant) {
        totalRedundant.merge(*rest.redundant);
    }

    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count();
    writeLine(out, intervalRecord("summary", static_cast<unsigned>(elapsed), totalFrames, totalPrimary,
                                  options.redundant ? &totalRedundant : nullptr,
                                  std::chrono::system_clock::now()));

    receiver.reset();
    if (totalFrames.total() == 0) {
        std::cerr << "Fehler: Keine Bilder empfangen (Adresse, Port, IGMP, Format prüfen).\n";
        return 3;
    }
    return 0;
}
