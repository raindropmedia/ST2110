#include "core/MonitorHub.h"

#include "core/Report.h"
#include "core/System10.h"

#include <algorithm>
#include <arpa/inet.h>

namespace st2110 {

using nlohmann::json;

namespace {

constexpr size_t kMaxEvents = 500;
constexpr size_t kMaxSeriesPoints = 1200;

uint32_t parseIp(const std::string& text) {
    in_addr addr{};
    if (inet_pton(AF_INET, text.c_str(), &addr) != 1) {
        return 0;
    }
    return ntohl(addr.s_addr);
}

} // namespace

MonitorHub::MonitorHub(HubConfig config, Clock clock)
    : config_(std::move(config)), clock_(std::move(clock)), startWall_(std::chrono::system_clock::now()) {
    if (!clock_) {
        const auto start = std::chrono::steady_clock::now();
        clock_ = [start] {
            return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        };
    }
    startSec_ = clock_();
    discoveryStatus_ = json{
        {"interface", config_.discoverInterface.empty() ? json(nullptr) : json(config_.discoverInterface)},
        {"active", false}};
}

std::string MonitorHub::nextIdLocked(Media media) {
    while (true) {
        const std::string id = media == Media::Video   ? "v" + std::to_string(nextVideo_++)
                               : media == Media::Audio ? "a" + std::to_string(nextAudio_++)
                               : media == Media::Anc   ? "d" + std::to_string(nextAnc_++)
                                                       : "h" + std::to_string(nextHbrmt_++);
        const bool used =
            std::any_of(flows_.begin(), flows_.end(), [&](const auto& f) { return f->id() == id; });
        if (!used) {
            return id;
        }
    }
}

std::shared_ptr<FlowMonitor> MonitorHub::addFlow(FlowConfig config, std::string& error) {
    std::vector<std::string> errors, warnings;
    validateFlow(config, errors, warnings);
    if (!errors.empty()) {
        error = errors.front();
        return nullptr;
    }
    std::shared_ptr<FlowMonitor> flow;
    {
        std::lock_guard lock(mutex_);
        if (flows_.size() >= config_.maxFlows) {
            error = "Höchstzahl gleichzeitiger Flows erreicht (" + std::to_string(config_.maxFlows) +
                    ", --max-flows).";
            return nullptr;
        }
        for (const auto& f : flows_) {
            if (f->matches(config.primary().ip, config.primary().udpPort) ||
                (config.redundant() && f->matches(config.redundant()->ip, config.redundant()->udpPort))) {
                error = "Adresse wird bereits von „" + f->config().label + "“ überwacht.";
                return nullptr;
            }
            if (!config.id.empty() && f->id() == config.id) {
                error = "ID " + config.id + " ist vergeben.";
                return nullptr;
            }
        }
        if (config.id.empty()) {
            config.id = nextIdLocked(config.media());
        }
        if (config.label.empty()) {
            config.label = std::string(config.video   ? "Video "
                                       : config.audio ? "Ton "
                                       : config.anc   ? "Zusatzdaten "
                                                      : "ST 2022-6 ") +
                           config.primary().ip;
        }
        const std::string label = config.label;
        auto sink = [this, label](const std::string& id, const std::string& level, const std::string& text) {
            addEvent(level, label + ": " + text, id);
        };
        auto clock = clock_;
        flow = std::make_shared<FlowMonitor>(config, sink, clock);
        flows_.push_back(flow);
        addEventLocked("info",
                       "Flow „" + label + "“ hinzugefügt (" + config.formatName() + ", " +
                           config.primary().ip + ":" + std::to_string(config.primary().udpPort) + ")",
                       config.id);
    }
    return flow;
}

bool MonitorHub::removeFlow(const std::string& id) {
    std::lock_guard lock(mutex_);
    auto it = std::find_if(flows_.begin(), flows_.end(), [&](const auto& f) { return f->id() == id; });
    if (it == flows_.end()) {
        return false;
    }
    addEventLocked("info", "Flow „" + (*it)->config().label + "“ entfernt", id);
    flows_.erase(it);
    return true;
}

std::shared_ptr<FlowMonitor> MonitorHub::flow(const std::string& id) const {
    std::lock_guard lock(mutex_);
    for (const auto& f : flows_) {
        if (f->id() == id) {
            return f;
        }
    }
    return nullptr;
}

std::vector<std::shared_ptr<FlowMonitor>> MonitorHub::flows() const {
    std::lock_guard lock(mutex_);
    return flows_;
}

void MonitorHub::addEvent(const std::string& level, const std::string& text, const std::string& flowId) {
    std::lock_guard lock(mutex_);
    addEventLocked(level, text, flowId);
}

void MonitorHub::addEventLocked(const std::string& level, const std::string& text,
                                const std::string& flowId) {
    const double t = now();
    const auto wall = startWall_ + std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                       std::chrono::duration<double>(t));
    events_.push_back(Event{t, isoTimestamp(wall), level, text, flowId});
    while (events_.size() > kMaxEvents) {
        events_.pop_front();
    }
}

void MonitorHub::onPtpSync(int64_t deltaNs, int16_t utcOffset) {
    std::lock_guard lock(mutex_);
    const double t = now();
    ptp_.onSync(t, deltaNs, utcOffset);
    checkPtpLocked(t);
}

void MonitorHub::checkPtpLocked(double t) {
    if (!config_.ptp && !config_.demo) {
        return;
    }
    const PtpState state = ptp_.state(t);
    if (ptpStateKnown_ && state == lastPtpState_) {
        return;
    }
    if (!ptpStateKnown_ && state != PtpState::Locked) {
        // Anlaufphase: "synchronisiert" nicht melden, "kein Sync" erst nach einigen Sekunden.
        if (state == PtpState::NoSync && t >= 5.0) {
            addEventLocked("error", "Kein PTP-Sync vom Grandmaster", {});
            ptpStateKnown_ = true;
            lastPtpState_ = state;
        }
        return;
    }
    switch (state) {
    case PtpState::Locked:
        addEventLocked("info", "PTP eingerastet (Korrektur < 1000 ns)", {});
        break;
    case PtpState::Syncing:
        addEventLocked("warn",
                       "PTP nicht eingerastet (Korrektur " + std::to_string(ptp_.lastDeltaNs()) + " ns)", {});
        break;
    case PtpState::NoSync:
        addEventLocked("error", "Kein PTP-Sync vom Grandmaster", {});
        break;
    }
    lastPtpState_ = state;
    ptpStateKnown_ = true;
}

void MonitorHub::onDiscoveredPacket(const PacketInfo& packet, uint32_t count, uint64_t bytes) {
    std::lock_guard lock(mutex_);
    const double t = now();
    discovered_.observe(packet, t, count, bytes);
    if (t - lastExpire_ > 5.0) {
        discovered_.expire(t);
        sap_.expire(t);
        lastExpire_ = t;
    }
}

void MonitorHub::onSapMessage(const SapMessage& message) {
    std::lock_guard lock(mutex_);
    const bool known = sap_.find(message.origin + "/" + std::to_string(message.hash)) != nullptr;
    sap_.add(message, now());
    if (!known && !message.deletion) {
        const auto* e = sap_.find(message.origin + "/" + std::to_string(message.hash));
        addEventLocked("info",
                       "SAP: Sitzung „" + (e != nullptr ? e->name : std::string("?")) + "“ von " +
                           message.origin + " angekündigt",
                       {});
    }
}

void MonitorHub::setDiscoveryStatus(json status) {
    std::lock_guard lock(mutex_);
    discoveryStatus_ = std::move(status);
}

std::string MonitorHub::announcedSdp(const std::string& key) const {
    std::lock_guard lock(mutex_);
    const auto* e = sap_.find(key);
    return e != nullptr ? e->sdp : std::string{};
}

void MonitorHub::reset() {
    for (const auto& f : flows()) {
        f->reset();
    }
    std::lock_guard lock(mutex_);
    ptp_.resetTotals();
    events_.clear();
    addEventLocked("info", "Messwerte zurückgesetzt", {});
}

std::string MonitorHub::monitoredIdFor(const std::vector<std::shared_ptr<FlowMonitor>>& flows, uint32_t ip,
                                       uint16_t port) const {
    const std::string text = ipv4ToString(ip);
    for (const auto& f : flows) {
        if (f->matches(text, port)) {
            return f->id();
        }
    }
    return {};
}

json MonitorHub::flowsJson() {
    const auto list = flows();
    json monitored = json::array();
    for (const auto& f : list) {
        monitored.push_back(f->summary());
    }
    json discovered = json::array();
    json announced = json::array();
    json status;
    {
        std::lock_guard lock(mutex_);
        const double t = now();
        auto entries = discovered_.list(t);
        std::sort(entries.begin(), entries.end(), [](const DiscoveredFlow& a, const DiscoveredFlow& b) {
            return std::tie(a.dstIp, a.dstPort, a.srcIp) < std::tie(b.dstIp, b.dstPort, b.srcIp);
        });
        for (const auto& d : entries) {
            discovered.push_back(discoveredFlowToJson(d, t, monitoredIdFor(list, d.dstIp, d.dstPort)));
        }
        for (const auto& [key, e] : sap_.entries()) {
            json media = json::array();
            for (const auto& f : e.flows) {
                const uint32_t ip = parseIp(f.primary().ip);
                media.push_back(json{{"kind", f.kind()},
                                     {"label", f.label},
                                     {"format", f.formatName()},
                                     {"address", f.primary().ip + ":" + std::to_string(f.primary().udpPort)},
                                     {"redundant", f.redundant().has_value()},
                                     {"monitored", [&]() -> json {
                                          auto id = monitoredIdFor(list, ip, f.primary().udpPort);
                                          return id.empty() ? json(nullptr) : json(id);
                                      }()}});
            }
            announced.push_back(json{{"key", key},
                                     {"name", e.name},
                                     {"origin", e.origin},
                                     {"media", media},
                                     {"errors", e.errors},
                                     {"announcements", e.announcements},
                                     {"lastSeenSec", t - e.lastSeen}});
        }
        status = discoveryStatus_;
    }
    return json{{"monitored", monitored},
                {"maxFlows", config_.maxFlows},
                {"discovered", discovered},
                {"announced", announced},
                {"discovery", status}};
}

json MonitorHub::ptpJson(double windowSec) {
    return ptp_.toJson(now(), windowSec, kMaxSeriesPoints);
}

json MonitorHub::eventsJsonLocked(size_t limit) const {
    json events = json::array();
    for (auto it = events_.rbegin(); it != events_.rend() && events.size() < limit; ++it) {
        events.push_back(json{{"t", it->t},
                              {"time", it->time},
                              {"level", it->level},
                              {"text", it->text},
                              {"flow", it->flowId.empty() ? json(nullptr) : json(it->flowId)}});
    }
    return events;
}

json MonitorHub::snapshot(const std::string& flowId, double windowSec) {
    const auto list = flows();
    json summaries = json::array();
    std::shared_ptr<FlowMonitor> selected;
    for (const auto& f : list) {
        summaries.push_back(f->summary());
        if (f->id() == flowId) {
            selected = f;
        }
    }
    if (!selected && !list.empty() && flowId.empty()) {
        selected = list.front();
    }
    json detail = selected ? selected->snapshot(windowSec) : json(nullptr);
    std::lock_guard lock(mutex_);
    const double t = now();
    checkPtpLocked(t);
    const json ptp = ptpJson(windowSec);
    if (detail.is_object()) {
        detail["system10"] = buildSystem10(detail, ptp, config_.ptp, config_.hwTimestamp);
    }
    return json{{"time", isoTimestamp(std::chrono::system_clock::now())},
                {"uptimeSec", t},
                {"demo", config_.demo},
                {"hwTimestamp", config_.hwTimestamp},
                {"ptpEnabled", config_.ptp},
                {"ptp", ptp},
                {"flows", summaries},
                {"flow", detail},
                {"events", eventsJsonLocked(200)}};
}

json MonitorHub::startRecord() {
    json flows = json::object();
    for (const auto& f : this->flows()) {
        const auto& c = f->config();
        json paths{{"P", flowAddressToJson(c.primary())}};
        if (c.redundant()) {
            paths["R"] = flowAddressToJson(*c.redundant());
        }
        flows[c.id] = json{{"label", c.label},
                           {"kind", c.kind()},
                           {"format", c.formatName()},
                           {"payloadType", c.payloadType()},
                           {"paths", paths}};
    }
    return json{{"type", "start"},      {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"demo", config_.demo}, {"hwTimestamp", config_.hwTimestamp},
                {"ptp", config_.ptp},   {"flows", flows}};
}

json MonitorHub::takeIntervalRecord() {
    json flows = json::object();
    for (const auto& f : this->flows()) {
        flows[f->id()] = f->takeIntervalRecord();
    }
    std::lock_guard lock(mutex_);
    const double t = now();
    checkPtpLocked(t);
    return json{{"type", "interval"},
                {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"uptimeSec", t},
                {"flows", flows},
                {"ptp",
                 {{"state", ptpStateName(ptp_.state(t))},
                  {"syncs", ptp_.syncCount()},
                  {"deltaNs", rangeToJson(ptp_.takeInterval())}}}};
}

json MonitorHub::summaryRecord() {
    json flows = json::object();
    for (const auto& f : this->flows()) {
        flows[f->id()] = f->summaryRecord();
    }
    std::lock_guard lock(mutex_);
    const double t = now();
    return json{{"type", "summary"},
                {"time", isoTimestamp(std::chrono::system_clock::now())},
                {"durationSec", t},
                {"flows", flows},
                {"ptp",
                 {{"state", ptpStateName(ptp_.state(t))},
                  {"syncs", ptp_.syncCount()},
                  {"deltaNs", rangeToJson(ptp_.totalDelta())}}}};
}

uint64_t MonitorHub::received() const {
    uint64_t total = 0;
    for (const auto& f : flows()) {
        total += f->received();
    }
    return total;
}

} // namespace st2110
