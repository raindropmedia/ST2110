#include "core/TestSender.h"

#include "core/FrameRate.h"

#include <algorithm>

namespace st2110 {

using nlohmann::json;

const char* txStateName(TxState state) {
    switch (state) {
    case TxState::Stopped:
        return "stopped";
    case TxState::Starting:
        return "starting";
    case TxState::Sending:
        return "sending";
    case TxState::Stopping:
        return "stopping";
    case TxState::Error:
        return "error";
    }
    return "stopped";
}

// Ein Eintrag. Alle Felder außer starter stehen unter TestSender::mutex_; der Startvorgang selbst
// (Backend::open) läuft ohne Sperre in einem eigenen Thread.
struct TestSender::Entry {
    std::string id;
    FlowConfig flow;
    TxState state = TxState::Stopped;
    std::string message;
    bool wantRun = false; // false im Zustand Starting: nach dem Start sofort wieder stoppen
    std::unique_ptr<TxBackend::Session> session;
    std::thread starter;
};

TestSender::TestSender(TxBackend& backend, unsigned maxFlows, std::string portName, std::string localIp)
    : backend_(backend), maxFlows_(maxFlows), portName_(std::move(portName)), localIp_(std::move(localIp)) {}

TestSender::~TestSender() {
    std::vector<std::shared_ptr<Entry>> entries;
    {
        std::lock_guard lock(mutex_);
        entries = entries_;
        for (auto& e : entries) {
            e->wantRun = false;
        }
    }
    for (auto& e : entries) {
        if (e->starter.joinable()) {
            e->starter.join();
        }
        e->session.reset();
    }
}

std::shared_ptr<TestSender::Entry> TestSender::find(const std::string& id) const {
    auto it = std::find_if(entries_.begin(), entries_.end(), [&](const auto& e) { return e->id == id; });
    return it == entries_.end() ? nullptr : *it;
}

bool TestSender::validate(const FlowConfig& flow, const std::string& ignoreId, std::string& error) const {
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
    validateFlow(flow, errors, warnings);
    if (flow.redundant()) {
        errors.push_back("Der Testsender sendet ohne ST 2022-7 (kein red=).");
    }
    for (const auto& e : entries_) {
        if (e->id != ignoreId && e->flow.primary().ip == flow.primary().ip &&
            e->flow.primary().udpPort == flow.primary().udpPort) {
            errors.push_back("Das Ziel " + flow.primary().ip + ":" + std::to_string(flow.primary().udpPort) +
                             " wird schon von " + e->id + " benutzt.");
        }
    }
    if (!errors.empty()) {
        error = errors.front();
        return false;
    }
    return true;
}

bool TestSender::add(FlowConfig flow, bool startNow, std::string& id, std::string& error) {
    std::shared_ptr<Entry> entry;
    {
        std::lock_guard lock(mutex_);
        if (entries_.size() >= maxFlows_) {
            error = "Höchstzahl erreicht (" + std::to_string(maxFlows_) + " Testsender, --max-tx-flows).";
            return false;
        }
        if (!validate(flow, {}, error)) {
            return false;
        }
        entry = std::make_shared<Entry>();
        entry->id = "t" + std::to_string(nextId_++);
        if (flow.label.empty()) {
            flow.label = (flow.video ? "Testbild " : "Testton ") + entry->id;
        }
        entry->flow = std::move(flow);
        entries_.push_back(entry);
        id = entry->id;
    }
    if (startNow) {
        return start(id, error);
    }
    return true;
}

bool TestSender::update(const std::string& id, FlowConfig flow, std::string& error) {
    std::lock_guard lock(mutex_);
    auto entry = find(id);
    if (!entry) {
        error = "Testsender " + id + " nicht gefunden.";
        return false;
    }
    if (entry->state != TxState::Stopped && entry->state != TxState::Error) {
        error = "Einstellungen lassen sich nur ändern, solange der Sender gestoppt ist.";
        return false;
    }
    if (flow.video.has_value() != entry->flow.video.has_value()) {
        error = "Video und Ton lassen sich nicht umwandeln: Eintrag löschen und neu anlegen.";
        return false;
    }
    if (!validate(flow, id, error)) {
        return false;
    }
    if (flow.label.empty()) {
        flow.label = entry->flow.label;
    }
    entry->flow = std::move(flow);
    entry->state = TxState::Stopped;
    entry->message.clear();
    return true;
}

// Startet den Hintergrundthread für einen Eintrag im Zustand Starting.
void TestSender::launch(const std::shared_ptr<Entry>& entry) {
    if (entry->starter.joinable()) {
        entry->starter.join(); // der vorige Startvorgang ist fertig (Zustand war Stopped oder Error)
    }
    FlowConfig flow = entry->flow;
    entry->starter = std::thread([this, entry, flow] {
        std::string error;
        std::unique_ptr<TxBackend::Session> session = backend_.open(flow, error);
        std::unique_ptr<TxBackend::Session> discard;
        {
            std::lock_guard lock(mutex_);
            if (!entry->wantRun) {
                discard = std::move(session); // zwischendurch gestoppt
                entry->state = TxState::Stopped;
                entry->message.clear();
            } else if (!session) {
                entry->state = TxState::Error;
                entry->message = error.empty() ? "Senden konnte nicht gestartet werden." : error;
            } else {
                entry->session = std::move(session);
                entry->state = TxState::Sending;
                entry->message.clear();
            }
        }
        discard.reset(); // ohne Sperre freigeben
    });
}

bool TestSender::start(const std::string& id, std::string& error) {
    std::lock_guard lock(mutex_);
    auto entry = find(id);
    if (!entry) {
        error = "Testsender " + id + " nicht gefunden.";
        return false;
    }
    switch (entry->state) {
    case TxState::Sending:
    case TxState::Starting:
        return true; // läuft schon bzw. startet
    case TxState::Stopping:
        error = "Der Sender stoppt gerade; danach erneut starten.";
        return false;
    case TxState::Stopped:
    case TxState::Error:
        break;
    }
    entry->state = TxState::Starting;
    entry->message.clear();
    entry->wantRun = true;
    launch(entry);
    return true;
}

bool TestSender::stop(const std::string& id, std::string& error) {
    std::unique_ptr<TxBackend::Session> session;
    {
        std::lock_guard lock(mutex_);
        auto entry = find(id);
        if (!entry) {
            error = "Testsender " + id + " nicht gefunden.";
            return false;
        }
        switch (entry->state) {
        case TxState::Starting:
            entry->wantRun = false;
            entry->state = TxState::Stopping;
            return true;
        case TxState::Sending:
            session = std::move(entry->session);
            entry->wantRun = false;
            entry->state = TxState::Stopped;
            break;
        case TxState::Error:
            entry->state = TxState::Stopped;
            entry->message.clear();
            return true;
        case TxState::Stopped:
        case TxState::Stopping:
            return true;
        }
    }
    session.reset(); // Senden beenden (wartet auf den Sendethread), ohne Sperre
    return true;
}

bool TestSender::remove(const std::string& id, std::string& error) {
    std::unique_ptr<TxBackend::Session> session;
    std::thread starter;
    {
        std::lock_guard lock(mutex_);
        auto entry = find(id);
        if (!entry) {
            error = "Testsender " + id + " nicht gefunden.";
            return false;
        }
        if (entry->state == TxState::Starting || entry->state == TxState::Stopping) {
            error = "Der Sender startet oder stoppt gerade; danach erneut versuchen.";
            return false;
        }
        session = std::move(entry->session);
        starter = std::move(entry->starter); // fertig: Zustand ist nicht Starting
        entries_.erase(std::remove(entries_.begin(), entries_.end(), entry), entries_.end());
    }
    session.reset(); // Senden beenden, ohne Sperre
    if (starter.joinable()) {
        starter.join();
    }
    return true;
}

std::vector<TxEntryInfo> TestSender::list() const {
    std::lock_guard lock(mutex_);
    std::vector<TxEntryInfo> out;
    for (const auto& e : entries_) {
        out.push_back(TxEntryInfo{e->id, e->flow, e->state, e->message});
    }
    return out;
}

nlohmann::json TestSender::toJson() const {
    json flows = json::array();
    for (const auto& info : list()) {
        const auto& f = info.flow;
        json item{{"id", info.id},
                  {"kind", f.video ? "video" : "audio"},
                  {"label", f.label},
                  {"format", f.formatName()},
                  {"ip", f.primary().ip},
                  {"udpPort", f.primary().udpPort},
                  {"payloadType", f.payloadType()},
                  {"state", txStateName(info.state)},
                  {"message", info.message}};
        if (f.video) {
            const auto& v = f.video->format;
            item["video"] = json{{"width", v.width},
                                 {"height", v.height},
                                 {"rate", frameRateName(v.rate)},
                                 {"interlaced", v.interlaced}};
        } else {
            const auto& a = f.audio->format;
            item["audio"] = json{{"sampleRate", a.sampleRate},
                                 {"channels", a.channels},
                                 {"bitDepth", a.bitDepth},
                                 {"ptime", packetTimeName(a.ptime)}};
        }
        flows.push_back(std::move(item));
    }
    return json{{"port", portName_}, {"localIp", localIp_}, {"maxFlows", maxFlows_}, {"flows", flows}};
}

} // namespace st2110
